#include "PdfRenderer.h"
#include "PdfiumLoader.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace pdfv
{
namespace
{
// PDFium is not thread-safe. Every plugin instance owns a worker thread, so all
// PDFium calls in the process are serialised through this one mutex.
std::mutex& PdfiumMutex()
{
	static std::mutex m;
	return m;
}

fs::path PathFromUtf8( const std::string& s )
{
#if defined( __cpp_char8_t )
	return fs::path( std::u8string( s.begin(), s.end() ) );
#else
	return fs::u8path( s );
#endif
}

std::string DescribePdfiumError( unsigned long code )
{
	switch( code )
	{
	case FPDF_ERR_FILE: return "file not found or could not be opened";
	case FPDF_ERR_FORMAT: return "not a PDF or file is corrupted";
	case FPDF_ERR_PASSWORD: return "password required or incorrect";
	case FPDF_ERR_SECURITY: return "unsupported security scheme";
	case FPDF_ERR_PAGE: return "page not found or content error";
	default: return "unknown error";
	}
}
}// namespace

int PdfRenderer::ScaleStepFor( float pixelsPerPoint )
{
	pixelsPerPoint = std::max( pixelsPerPoint, 1.0f / 64.0f );
	// Round UP to the next quarter octave so we always supersample a little.
	return (int)std::ceil( 4.0f * std::log2( pixelsPerPoint ) - 1e-4f );
}

float PdfRenderer::PixelsPerPoint( int scaleStep )
{
	return std::exp2( scaleStep / 4.0f );
}

PdfRenderer::PdfRenderer()
{
	thread = std::thread( [ this ] { ThreadMain(); } );
}

PdfRenderer::~PdfRenderer()
{
	{
		std::lock_guard< std::mutex > lock( mutex );
		quit = true;
	}
	cv.notify_all();
	if( thread.joinable() )
		thread.join();
	CloseDoc();
}

void PdfRenderer::Open( const std::string& utf8Path, const std::string& pwd )
{
	{
		std::lock_guard< std::mutex > lock( mutex );
		path        = utf8Path;
		password    = pwd;
		openPending = true;
	}
	cv.notify_all();
}

void PdfRenderer::Reload()
{
	{
		std::lock_guard< std::mutex > lock( mutex );
		openPending = true;
	}
	cv.notify_all();
}

void PdfRenderer::SetAutoReload( bool enabled )
{
	std::lock_guard< std::mutex > lock( mutex );
	autoReload = enabled;
}

void PdfRenderer::Request( int page, int scaleStep, bool transparent )
{
	bool changed;
	{
		std::lock_guard< std::mutex > lock( mutex );
		changed = !hasRequest || request.page != page || request.scaleStep != scaleStep || request.transparent != transparent;
		request = { page, scaleStep, transparent };
		hasRequest = true;
	}
	if( changed )
		cv.notify_all();
}

DocInfo PdfRenderer::GetDocInfo() const
{
	std::lock_guard< std::mutex > lock( mutex );
	return docInfo;
}

std::shared_ptr< const PageBitmap > PdfRenderer::Find( uint64_t gen, int page, int step, bool transparent ) const
{
	std::lock_guard< std::mutex > lock( mutex );
	for( const auto& b : cache )
		if( b->docGen == gen && b->page == page && b->scaleStep == step && b->transparent == transparent )
			return b;
	return nullptr;
}

std::shared_ptr< const PageBitmap > PdfRenderer::FindAny( uint64_t gen, int page, int step, bool transparent ) const
{
	std::lock_guard< std::mutex > lock( mutex );
	std::shared_ptr< const PageBitmap > best;
	for( const auto& b : cache )
	{
		if( b->docGen != gen || b->page != page || b->transparent != transparent )
			continue;
		if( !best || std::abs( b->scaleStep - step ) < std::abs( best->scaleStep - step ) )
			best = b;
	}
	return best;
}

bool PdfRenderer::IsCachedLocked( const Job& job ) const
{
	for( const auto& b : cache )
		if( b->docGen == docInfo.generation && b->page == job.page && b->scaleStep == job.scaleStep && b->transparent == job.transparent )
			return true;
	return false;
}

void PdfRenderer::StoreLocked( std::shared_ptr< PageBitmap > bmp )
{
	cache.push_front( std::move( bmp ) );
	size_t total = 0;
	for( const auto& b : cache )
		total += b->rgba.size();
	// Evict least recently produced, but always keep the newest few.
	while( total > cacheBudget && cache.size() > 3 )
	{
		total -= cache.back()->rgba.size();
		cache.pop_back();
	}
}

int64_t PdfRenderer::FileStamp( const std::string& p ) const
{
	std::error_code ec;
	auto t = fs::last_write_time( PathFromUtf8( p ), ec );
	if( ec )
		return 0;
	auto sz = fs::file_size( PathFromUtf8( p ), ec );
	return (int64_t)t.time_since_epoch().count() ^ ( ec ? 0 : (int64_t)sz << 1 );
}

void PdfRenderer::CloseDoc()
{
	if( doc )
	{
		const PdfiumApi* api = GetPdfium();
		std::lock_guard< std::mutex > g( PdfiumMutex() );
		api->CloseDocument( (FPDF_DOCUMENT)doc );
		doc = nullptr;
	}
	fileData.clear();
	fileData.shrink_to_fit();
}

bool PdfRenderer::DoOpen( std::unique_lock< std::mutex >& lock )
{
	const std::string p   = path;
	const std::string pwd = password;
	docInfo.loading       = true;
	lock.unlock();

	DocInfo result;
	std::string err;
	const PdfiumApi* api = GetPdfium( &err );

	CloseDoc();
	int64_t stamp = 0;

	if( p.empty() )
	{
		// No file selected: not an error, just nothing to show.
	}
	else if( !api )
	{
		result.error = err;
	}
	else
	{
		// Read the whole file ourselves: handles UTF-8/Unicode paths on every
		// platform and does not keep the PDF locked, so it can be overwritten
		// while Resolume is running (see Auto Reload).
		stamp = FileStamp( p );
		std::ifstream in( PathFromUtf8( p ), std::ios::binary );
		if( !in )
		{
			result.error = "Cannot open file: " + p;
		}
		else
		{
			in.seekg( 0, std::ios::end );
			std::streamoff size = in.tellg();
			in.seekg( 0, std::ios::beg );
			std::vector< uint8_t > data( size > 0 ? (size_t)size : 0 );
			if( size > 0 )
				in.read( (char*)data.data(), size );

			std::lock_guard< std::mutex > g( PdfiumMutex() );
			FPDF_DOCUMENT d = api->LoadMemDocument64( data.data(), data.size(), pwd.empty() ? nullptr : pwd.c_str() );
			if( !d )
			{
				result.error = "PDF: " + DescribePdfiumError( api->GetLastError() );
			}
			else
			{
				int n = api->GetPageCount( d );
				for( int i = 0; i < n; ++i )
				{
					double w = 0, h = 0;
					PageSize ps;
					if( api->GetPageSizeByIndex( d, i, &w, &h ) && w > 0 && h > 0 )
					{
						ps.w = (float)w;
						ps.h = (float)h;
					}
					result.pages.push_back( ps );
				}
				if( n <= 0 )
				{
					result.error = "PDF has no pages";
					api->CloseDocument( d );
				}
				else
				{
					doc      = d;
					fileData = std::move( data );
				}
			}
		}
	}

	lock.lock();
	openedPath       = p;
	openedStamp      = stamp;
	result.generation = docInfo.generation + 1;
	result.loading    = false;
	docInfo           = std::move( result );
	cache.clear();
	return doc != nullptr;
}

std::shared_ptr< PageBitmap > PdfRenderer::RenderPage( const Job& job, uint64_t gen )
{
	const PdfiumApi* api = GetPdfium();
	if( !api || !doc )
		return nullptr;

	PageSize ps;
	{
		std::lock_guard< std::mutex > lock( mutex );
		if( job.page < 0 || job.page >= (int)docInfo.pages.size() )
			return nullptr;
		ps = docInfo.pages[ job.page ];
	}

	float ppp   = PixelsPerPoint( job.scaleStep );
	float w     = ps.w * ppp;
	float h     = ps.h * ppp;
	float limit = (float)std::max( 256, maxTextureSize.load() );
	float fit   = std::min( 1.0f, limit / std::max( w, h ) );
	int iw      = std::max( 1, (int)std::lround( w * fit ) );
	int ih      = std::max( 1, (int)std::lround( h * fit ) );

	auto bmp         = std::make_shared< PageBitmap >();
	bmp->docGen      = gen;
	bmp->page        = job.page;
	bmp->scaleStep   = job.scaleStep;
	bmp->transparent = job.transparent;
	bmp->width       = iw;
	bmp->height      = ih;
	try
	{
		bmp->rgba.assign( (size_t)iw * ih * 4, 0 );
	}
	catch( const std::bad_alloc& )
	{
		return nullptr;
	}

	std::lock_guard< std::mutex > g( PdfiumMutex() );
	FPDF_PAGE page = api->LoadPage( (FPDF_DOCUMENT)doc, job.page );
	if( !page )
		return nullptr;
	FPDF_BITMAP bitmap = api->BitmapCreateEx( iw, ih, FPDFBitmap_BGRA, bmp->rgba.data(), iw * 4 );
	if( !bitmap )
	{
		api->ClosePage( page );
		return nullptr;
	}
	if( !job.transparent )
		api->BitmapFillRect( bitmap, 0, 0, iw, ih, 0xFFFFFFFF );
	// FPDF_REVERSE_BYTE_ORDER turns BGRA into RGBA so it uploads straight to GL.
	api->RenderPageBitmap( bitmap, page, 0, 0, iw, ih, 0, FPDF_ANNOT | FPDF_REVERSE_BYTE_ORDER );
	api->BitmapDestroy( bitmap );
	api->ClosePage( page );
	return bmp;
}

void PdfRenderer::ThreadMain()
{
	using clock         = std::chrono::steady_clock;
	auto lastStampCheck = clock::now();

	std::unique_lock< std::mutex > lock( mutex );
	while( !quit )
	{
		if( openPending )
		{
			openPending = false;
			DoOpen( lock );
			continue;
		}

		// Auto reload: poll the file's timestamp about once a second.
		if( autoReload && !openedPath.empty() && clock::now() - lastStampCheck > std::chrono::seconds( 1 ) )
		{
			lastStampCheck      = clock::now();
			std::string p       = openedPath;
			int64_t known       = openedStamp;
			lock.unlock();
			int64_t now = FileStamp( p );
			lock.lock();
			if( now != 0 && now != known && p == openedPath && !openPending )
			{
				// Give whoever is writing the file a moment to finish.
				lock.unlock();
				std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
				lock.lock();
				openPending = true;
				continue;
			}
		}

		// Pick the next job: the current request first, then neighbours.
		bool haveJob = false;
		Job job{};
		if( doc && hasRequest )
		{
			const int count = (int)docInfo.pages.size();
			const int offsets[] = { 0, 1, -1, 2 };
			for( int off : offsets )
			{
				Job j{ request.page + off, request.scaleStep, request.transparent };
				if( j.page < 0 || j.page >= count )
					continue;
				if( !IsCachedLocked( j ) )
				{
					job     = j;
					haveJob = true;
					break;
				}
			}
		}

		if( !haveJob )
		{
			cv.wait_for( lock, std::chrono::milliseconds( 250 ) );
			continue;
		}

		const uint64_t gen = docInfo.generation;
		lock.unlock();
		auto bmp = RenderPage( job, gen );
		lock.lock();
		if( bmp && gen == docInfo.generation )
			StoreLocked( std::move( bmp ) );
		else if( !bmp )
		{
			// Rendering failed (e.g. out of memory): store a 1x1 placeholder so we
			// don't spin on the same job forever.
			auto ph         = std::make_shared< PageBitmap >();
			ph->docGen      = gen;
			ph->page        = job.page;
			ph->scaleStep   = job.scaleStep;
			ph->transparent = job.transparent;
			ph->width = ph->height = 1;
			ph->rgba.assign( 4, 0 );
			if( gen == docInfo.generation )
				StoreLocked( std::move( ph ) );
		}
	}
}

}// namespace pdfv
