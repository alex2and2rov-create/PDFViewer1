// Offline test of the PDF worker: opens a PDF, renders pages through the same
// code path the plugin uses and writes them as PAM images (RGBA).
//   pdfviewer_test file.pdf out_prefix [pixelsPerPoint]
#include "PdfRenderer.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace pdfv;

static bool WritePam( const std::string& path, const PageBitmap& b )
{
	FILE* f = std::fopen( path.c_str(), "wb" );
	if( !f )
		return false;
	std::fprintf( f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", b.width, b.height );
	std::fwrite( b.rgba.data(), 1, b.rgba.size(), f );
	std::fclose( f );
	return true;
}

int main( int argc, char** argv )
{
	if( argc < 3 )
	{
		std::fprintf( stderr, "usage: %s file.pdf out_prefix [pixelsPerPoint]\n", argv[ 0 ] );
		return 2;
	}
	float ppp = argc > 3 ? std::stof( argv[ 3 ] ) : 1.0f;

	PdfRenderer r;
	r.Open( argv[ 1 ], "" );

	auto t0 = std::chrono::steady_clock::now();
	DocInfo info;
	do
	{
		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		info = r.GetDocInfo();
	} while( info.generation == 0 );
	if( !info.error.empty() )
	{
		std::printf( "ERROR: %s\n", info.error.c_str() );
		return 1;
	}
	std::printf( "pages: %zu (loaded in %.0f ms)\n", info.pages.size(),
	             std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() );

	const int step = PdfRenderer::ScaleStepFor( ppp );
	for( int p = 0; p < (int)info.pages.size(); ++p )
	{
		for( bool transparent : { false, true } )
		{
			t0 = std::chrono::steady_clock::now();
			std::shared_ptr< const PageBitmap > b;
			while( !( b = r.Find( info.generation, p, step, transparent ) ) )
			{
				r.Request( p, step, transparent );
				std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
			}
			double ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
			std::string out = std::string( argv[ 2 ] ) + "_p" + std::to_string( p + 1 ) + ( transparent ? "_alpha" : "" ) + ".pam";
			WritePam( out, *b );
			std::printf( "page %d %s: %.0fx%.0f pt -> %dx%d px (step %d) in %.0f ms -> %s\n", p + 1,
			             transparent ? "transparent" : "white", info.pages[ p ].w, info.pages[ p ].h, b->width, b->height,
			             step, ms, out.c_str() );
		}
	}
	return 0;
}
