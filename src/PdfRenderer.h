#pragma once
// Background PDF rasteriser.
//
// All PDFium work happens on a worker thread so the render thread never
// stalls: opening a 300-page deck or rasterising a 6K page can take hundreds
// of milliseconds. The render thread just posts "I want page N at scale S"
// every frame and picks up finished bitmaps from a small LRU cache.
//
// Neighbouring pages are pre-rendered while idle, so Next / Prev during a
// show is instant.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pdfv
{
struct PageSize
{
	float w = 612.0f;// points (1/72 inch)
	float h = 792.0f;
};

struct PageBitmap
{
	uint64_t docGen  = 0;
	int page         = 0;
	int scaleStep    = 0;
	bool transparent = false;
	int width        = 0;
	int height       = 0;
	std::vector< uint8_t > rgba;// top row first, straight (non-premultiplied) alpha
};

struct DocInfo
{
	uint64_t generation = 0;// bumped every time a document is (re)loaded
	std::vector< PageSize > pages;
	std::string error;// empty when OK
	bool loading = false;
};

class PdfRenderer
{
public:
	PdfRenderer();
	~PdfRenderer();

	// Asynchronous; result is visible through GetDocInfo().
	void Open( const std::string& utf8Path, const std::string& password );
	void Reload();
	void SetAutoReload( bool enabled );

	// Called every frame by the render thread with what it would like to show.
	void Request( int page, int scaleStep, bool transparent );

	DocInfo GetDocInfo() const;

	// Exact match from the cache, or nullptr.
	std::shared_ptr< const PageBitmap > Find( uint64_t docGen, int page, int scaleStep, bool transparent ) const;
	// Any cached bitmap of this page (prefers the closest scale), or nullptr.
	std::shared_ptr< const PageBitmap > FindAny( uint64_t docGen, int page, int scaleStep, bool transparent ) const;

	// Scale steps are quarter-octaves: pixelsPerPoint = 2^(step/4).
	static int ScaleStepFor( float pixelsPerPoint );
	static float PixelsPerPoint( int scaleStep );

	void SetMaxTextureSize( int size ) { maxTextureSize = size; }
	void SetCacheBudgetBytes( size_t bytes ) { cacheBudget = bytes; }

private:
	struct Job
	{
		int page;
		int scaleStep;
		bool transparent;
	};

	void ThreadMain();
	bool DoOpen( std::unique_lock< std::mutex >& lock );
	void CloseDoc();
	std::shared_ptr< PageBitmap > RenderPage( const Job& job, uint64_t gen );
	bool IsCachedLocked( const Job& job ) const;
	void StoreLocked( std::shared_ptr< PageBitmap > bmp );
	int64_t FileStamp( const std::string& path ) const;

	mutable std::mutex mutex;
	std::condition_variable cv;
	std::thread thread;
	bool quit = false;

	// Requests (guarded by mutex)
	std::string path, password;
	bool openPending   = false;
	bool autoReload    = false;
	bool hasRequest    = false;
	Job request{ 0, 0, false };

	// Document (worker only, except docInfo which is guarded)
	void* doc = nullptr;           // FPDF_DOCUMENT
	std::vector< uint8_t > fileData;// must outlive the FPDF_DOCUMENT
	std::string openedPath;
	int64_t openedStamp = 0;
	DocInfo docInfo;

	// Cache (guarded by mutex), most recently used at front
	std::list< std::shared_ptr< PageBitmap > > cache;
	size_t cacheBudget = size_t( 768 ) * 1024 * 1024;

	std::atomic< int > maxTextureSize{ 8192 };
};

}// namespace pdfv
