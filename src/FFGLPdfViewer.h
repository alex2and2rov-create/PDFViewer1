#pragma once
#include <FFGLSDK.h>
#include <chrono>
#include <memory>
#include <string>
#include "PdfRenderer.h"

class FFGLPdfViewer : public CFFGLPlugin
{
public:
	FFGLPdfViewer();
	~FFGLPdfViewer() override;

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;
	char* GetParameterDisplay( unsigned int index ) override;

private:
	struct Layer
	{
		GLuint tex       = 0;
		bool valid       = false;
		uint64_t docGen  = 0;
		int page         = -1;
		int scaleStep    = 0;
		bool transparent = false;
		pdfv::PageSize size;
	};
	struct Rect
	{
		float cx, cy, w, h;
		float pixelsPerPoint;
	};

	void SyncDocument();
	void GoToPage( int page );
	Rect ComputeRect( const pdfv::PageSize& ps ) const;
	void Upload( Layer& layer, const pdfv::PageBitmap& bmp, const pdfv::PageSize& ps );
	int Rotation() const;

	std::unique_ptr< pdfv::PdfRenderer > renderer;

	// Parameters
	std::string filePath;
	std::string password;
	bool fileDirty      = false;
	float pageValue     = 0.0f;// option value as sent by the host (0-based page)
	int fitMode         = 0;
	float zoom          = 1.0f;
	float panX          = 0.5f;
	float panY          = 0.5f;
	int rotation        = 0;
	int qualityIndex    = 1;
	float transition    = 0.25f;
	int pageBackground  = 0;// 0 = white, 1 = transparent
	bool invert         = false;
	bool autoReload     = false;
	float bg[ 4 ]       = { 0.0f, 0.0f, 0.0f, 1.0f };

	// Document state as seen by the render thread
	uint64_t knownGen = 0;
	int pageCount     = 0;
	std::vector< pdfv::PageSize > pageSizes;
	std::string lastError;

	// GL
	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;
	Layer cur, prev;
	bool fading = false;
	std::chrono::steady_clock::time_point fadeStart;
	int maxTextureSize = 8192;

	char displayBuffer[ 64 ] = {};
};
