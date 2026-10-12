#include "FFGLPdfViewer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ffglex;

enum ParamID : FFUInt32
{
	// File
	PT_FILE,
	PT_PASSWORD,
	PT_RELOAD,
	PT_AUTORELOAD,
	// Navigation
	PT_PAGE,
	PT_PREV,
	PT_NEXT,
	PT_FIRST,
	PT_LAST,
	PT_TRANSITION,
	// View
	PT_FIT,
	PT_ZOOM,
	PT_PANX,
	PT_PANY,
	PT_ROTATE,
	PT_QUALITY,
	// Look
	PT_PAGEBG,
	PT_INVERT,
	PT_BG_RED,
	PT_BG_GREEN,
	PT_BG_BLUE,
	PT_BG_ALPHA,
};

enum FitMode
{
	FIT_CONTAIN,
	FIT_FILL,
	FIT_WIDTH,
	FIT_HEIGHT,
	FIT_STRETCH
};

static const float kQuality[] = { 0.5f, 1.0f, 1.5f, 2.0f };

static CFFGLPluginInfo PluginInfo(
	PluginFactory< FFGLPdfViewer >,
	"PDFV",                                                // Plugin unique ID
	"PDF Viewer",                                          // Plugin name
	2,                                                     // API major version
	1,                                                     // API minor version
	1,                                                     // Plugin major version
	0,                                                     // Plugin minor version
	FF_SOURCE,                                             // Plugin type
	"Displays PDF documents page by page. Uses PDFium.",  // Description
	"PDF Viewer for Resolume (FFGL)"                       // About
);

static const char vertexShaderCode[] = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;
out vec2 uv;
void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

static const char fragmentShaderCode[] = R"(#version 410 core
uniform sampler2D curTex;
uniform sampler2D prevTex;
uniform vec2 resolution;
uniform vec4 curRect;   // center.xy, size.xy  (pixels, y down)
uniform vec4 prevRect;
uniform int  hasCur;
uniform int  hasPrev;
uniform int  rotation;  // 0..3, clockwise quarter turns
uniform float fade;     // 0 = prev only, 1 = cur only
uniform vec4 background;
uniform int  invert;

in vec2 uv;
out vec4 fragColor;

vec4 samplePage( sampler2D tex, vec4 rect, vec2 p )
{
	vec2 q = ( p - rect.xy ) / rect.zw + 0.5;
	if( q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0 )
		return vec4( 0.0 );
	vec2 t;
	if( rotation == 1 )      t = vec2( q.y, 1.0 - q.x );
	else if( rotation == 2 ) t = 1.0 - q;
	else if( rotation == 3 ) t = vec2( 1.0 - q.y, q.x );
	else                     t = q;
	vec4 c = texture( tex, t );
	if( invert != 0 )
		c.rgb = 1.0 - c.rgb;
	return c;
}

// "a over b" with straight (non-premultiplied) alpha, result premultiplied.
vec4 overPremul( vec4 a, vec4 b )
{
	vec3 rgb = a.rgb * a.a + b.rgb * b.a * ( 1.0 - a.a );
	return vec4( rgb, a.a + b.a * ( 1.0 - a.a ) );
}

void main()
{
	vec2 p = vec2( uv.x, 1.0 - uv.y ) * resolution;

	vec4 bgPremul = vec4( background.rgb * background.a, background.a );
	vec4 cur  = hasCur  != 0 ? overPremul( samplePage( curTex,  curRect,  p ), background ) : bgPremul;
	vec4 prev = hasPrev != 0 ? overPremul( samplePage( prevTex, prevRect, p ), background ) : bgPremul;
	vec4 outPremul = mix( prev, cur, fade );

	// Output straight alpha, like the FFGL example sources.
	fragColor = outPremul.a > 0.0 ? vec4( outPremul.rgb / outPremul.a, outPremul.a ) : vec4( 0.0 );
}
)";

FFGLPdfViewer::FFGLPdfViewer()
{
	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	// --- File
	SetFileParamInfo( PT_FILE, "PDF File", { "pdf" }, "" );
	SetParamInfo( PT_PASSWORD, "Password", FF_TYPE_TEXT, "" );
	SetParamInfof( PT_RELOAD, "Reload", FF_TYPE_EVENT );
	SetParamInfo( PT_AUTORELOAD, "Auto Reload", FF_TYPE_BOOLEAN, false );

	// --- Navigation
	SetOptionParamInfo( PT_PAGE, "Page", 1, 0.0f );
	SetParamElementInfo( PT_PAGE, 0, "-", 0.0f );
	SetParamInfof( PT_PREV, "Prev Page", FF_TYPE_EVENT );
	SetParamInfof( PT_NEXT, "Next Page", FF_TYPE_EVENT );
	SetParamInfof( PT_FIRST, "First Page", FF_TYPE_EVENT );
	SetParamInfof( PT_LAST, "Last Page", FF_TYPE_EVENT );
	SetParamInfo( PT_TRANSITION, "Transition", FF_TYPE_STANDARD, transition );
	SetParamRange( PT_TRANSITION, 0.0f, 3.0f );

	// --- View
	SetOptionParamInfo( PT_FIT, "Fit", 5, (float)fitMode );
	SetParamElementInfo( PT_FIT, 0, "Fit (letterbox)", 0.0f );
	SetParamElementInfo( PT_FIT, 1, "Fill (crop)", 1.0f );
	SetParamElementInfo( PT_FIT, 2, "Fit Width", 2.0f );
	SetParamElementInfo( PT_FIT, 3, "Fit Height", 3.0f );
	SetParamElementInfo( PT_FIT, 4, "Stretch", 4.0f );
	SetParamInfo( PT_ZOOM, "Zoom", FF_TYPE_STANDARD, zoom );
	SetParamRange( PT_ZOOM, 0.1f, 10.0f );
	SetParamInfo( PT_PANX, "Pan X", FF_TYPE_STANDARD, panX );
	SetParamInfo( PT_PANY, "Pan Y", FF_TYPE_STANDARD, panY );
	SetOptionParamInfo( PT_ROTATE, "Rotate", 4, 0.0f );
	SetParamElementInfo( PT_ROTATE, 0, "0", 0.0f );
	SetParamElementInfo( PT_ROTATE, 1, "90 CW", 1.0f );
	SetParamElementInfo( PT_ROTATE, 2, "180", 2.0f );
	SetParamElementInfo( PT_ROTATE, 3, "90 CCW", 3.0f );
	SetOptionParamInfo( PT_QUALITY, "Quality", 4, (float)qualityIndex );
	SetParamElementInfo( PT_QUALITY, 0, "Draft (0.5x)", 0.0f );
	SetParamElementInfo( PT_QUALITY, 1, "Normal (1x)", 1.0f );
	SetParamElementInfo( PT_QUALITY, 2, "High (1.5x)", 2.0f );
	SetParamElementInfo( PT_QUALITY, 3, "Max (2x)", 3.0f );

	// --- Look
	SetOptionParamInfo( PT_PAGEBG, "Page Paper", 2, 0.0f );
	SetParamElementInfo( PT_PAGEBG, 0, "White", 0.0f );
	SetParamElementInfo( PT_PAGEBG, 1, "Transparent", 1.0f );
	SetParamInfo( PT_INVERT, "Invert", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PT_BG_RED, "BG Red", FF_TYPE_RED, bg[ 0 ] );
	SetParamInfo( PT_BG_GREEN, "BG Green", FF_TYPE_GREEN, bg[ 1 ] );
	SetParamInfo( PT_BG_BLUE, "BG Blue", FF_TYPE_BLUE, bg[ 2 ] );
	SetParamInfo( PT_BG_ALPHA, "BG Alpha", FF_TYPE_ALPHA, bg[ 3 ] );

	for( FFUInt32 id : { PT_FILE, PT_PASSWORD, PT_RELOAD, PT_AUTORELOAD } )
		SetParamGroup( id, "File" );
	for( FFUInt32 id : { PT_FIT, PT_ZOOM, PT_PANX, PT_PANY, PT_ROTATE, PT_QUALITY } )
		SetParamGroup( id, "View" );
	for( FFUInt32 id : { PT_PAGEBG, PT_INVERT, PT_BG_RED, PT_BG_GREEN, PT_BG_BLUE, PT_BG_ALPHA } )
		SetParamGroup( id, "Look" );

	renderer = std::make_unique< pdfv::PdfRenderer >();
}

FFGLPdfViewer::~FFGLPdfViewer()
{
	renderer.reset();// joins the worker thread
}

FFResult FFGLPdfViewer::InitGL( const FFGLViewportStruct* vp )
{
	if( !shader.Compile( vertexShaderCode, fragmentShaderCode ) || !quad.Initialise() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	GLint maxTex = 0;
	glGetIntegerv( GL_MAX_TEXTURE_SIZE, &maxTex );
	maxTextureSize = std::max( 1024, std::min( (int)maxTex, 8192 ) );
	renderer->SetMaxTextureSize( maxTextureSize );

	return CFFGLPlugin::InitGL( vp );
}

FFResult FFGLPdfViewer::DeInitGL()
{
	shader.FreeGLResources();
	quad.Release();
	for( Layer* l : { &cur, &prev } )
	{
		if( l->tex )
			glDeleteTextures( 1, &l->tex );
		*l = Layer();
	}
	fading = false;
	return FF_SUCCESS;
}

int FFGLPdfViewer::Rotation() const
{
	return std::max( 0, std::min( 3, rotation ) );
}

FFGLPdfViewer::Rect FFGLPdfViewer::ComputeRect( const pdfv::PageSize& ps ) const
{
	const float W  = (float)std::max< GLuint >( 1, currentViewport.width );
	const float H  = (float)std::max< GLuint >( 1, currentViewport.height );
	const bool rot = ( Rotation() & 1 ) != 0;
	const float pw = rot ? ps.h : ps.w;// page size as it appears on screen
	const float ph = rot ? ps.w : ps.h;

	float sx = 1.0f, sy = 1.0f;
	switch( fitMode )
	{
	default:
	case FIT_CONTAIN: sx = sy = std::min( W / pw, H / ph ); break;
	case FIT_FILL: sx = sy = std::max( W / pw, H / ph ); break;
	case FIT_WIDTH: sx = sy = W / pw; break;
	case FIT_HEIGHT: sx = sy = H / ph; break;
	case FIT_STRETCH:
		sx = W / pw;
		sy = H / ph;
		break;
	}

	Rect r;
	r.w = pw * sx * zoom;
	r.h = ph * sy * zoom;
	// Pan 0 = left/top edge of the page aligned with the output's left/top edge,
	// 0.5 = centred, 1 = right/bottom aligned. Works both when zoomed in (scroll)
	// and when the page is smaller than the output (placement).
	r.cx             = W * 0.5f + ( 0.5f - panX ) * ( r.w - W );
	r.cy             = H * 0.5f + ( 0.5f - panY ) * ( r.h - H );
	r.pixelsPerPoint = std::max( sx, sy ) * zoom;
	return r;
}

void FFGLPdfViewer::GoToPage( int page )
{
	if( pageCount <= 0 )
		return;
	page      = std::max( 0, std::min( pageCount - 1, page ) );
	pageValue = (float)page;
	RaiseParamEvent( PT_PAGE, FF_EVENT_FLAG_VALUE );
}

void FFGLPdfViewer::SyncDocument()
{
	if( fileDirty )
	{
		fileDirty = false;
		renderer->Open( filePath, password );
	}

	pdfv::DocInfo info = renderer->GetDocInfo();
	if( info.generation == knownGen )
		return;
	knownGen  = info.generation;
	pageSizes = info.pages;

	std::vector< std::string > names;
	std::vector< float > values;
	if( !info.error.empty() )
	{
		if( info.error != lastError )
			FFGLLog::LogToHost( ( "PDF Viewer: " + info.error ).c_str() );
		names.push_back( "Error: " + info.error );
		values.push_back( 0.0f );
		pageSizes.clear();
	}
	else if( pageSizes.empty() )
	{
		names.push_back( "-" );
		values.push_back( 0.0f );
	}
	else
	{
		const int n = (int)pageSizes.size();
		for( int i = 0; i < n; ++i )
		{
			names.push_back( std::to_string( i + 1 ) + " / " + std::to_string( n ) );
			values.push_back( (float)i );
		}
	}
	lastError = info.error;
	pageCount = (int)pageSizes.size();
	SetParamElements( PT_PAGE, names, values, true );
	SetParamDisplayName( PT_PAGE, pageCount > 0 ? "Page (" + std::to_string( pageCount ) + ")" : std::string(), true );

	// A restored composition may have asked for page 7 before the file finished
	// loading; keep that request but clamp it to what the document has.
	if( pageCount > 0 && ( pageValue < 0.0f || pageValue > pageCount - 1 ) )
		GoToPage( (int)pageValue );
	else
		RaiseParamEvent( PT_PAGE, FF_EVENT_FLAG_VALUE );

	if( pageCount == 0 )
	{
		cur.valid  = false;
		prev.valid = false;
		fading     = false;
	}
}

void FFGLPdfViewer::Upload( Layer& layer, const pdfv::PageBitmap& bmp, const pdfv::PageSize& ps )
{
	if( !layer.tex )
		glGenTextures( 1, &layer.tex );
	{
		Scoped2DTextureBinding binding( layer.tex );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, bmp.width, bmp.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, bmp.rgba.data() );
		glGenerateMipmap( GL_TEXTURE_2D );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	}
	layer.valid       = true;
	layer.docGen      = bmp.docGen;
	layer.page        = bmp.page;
	layer.scaleStep   = bmp.scaleStep;
	layer.transparent = bmp.transparent;
	layer.size        = ps;
}

FFResult FFGLPdfViewer::ProcessOpenGL( ProcessOpenGLStruct* )
{
	using clock = std::chrono::steady_clock;
	SyncDocument();

	const bool transparentPaper = pageBackground == 1;
	const int page              = pageCount > 0 ? std::max( 0, std::min( pageCount - 1, (int)std::lround( pageValue ) ) ) : -1;

	if( page >= 0 )
	{
		const pdfv::PageSize& ps = pageSizes[ page ];
		Rect r                   = ComputeRect( ps );

		int step = pdfv::PdfRenderer::ScaleStepFor( r.pixelsPerPoint * kQuality[ std::max( 0, std::min( 3, qualityIndex ) ) ] );
		// Don't ask for scales the texture size limit would clamp anyway.
		const int maxStep = (int)std::floor( 4.0f * std::log2( (float)maxTextureSize / std::max( ps.w, ps.h ) ) );
		step              = std::min( step, maxStep );
		renderer->Request( page, step, transparentPaper );

		auto bmp = renderer->Find( knownGen, page, step, transparentPaper );
		if( !bmp && ( !cur.valid || cur.page != page || cur.docGen != knownGen || cur.transparent != transparentPaper ) )
			bmp = renderer->FindAny( knownGen, page, step, transparentPaper );

		const bool sameAsCur = cur.valid && bmp && cur.docGen == bmp->docGen && cur.page == bmp->page &&
		                       cur.scaleStep == bmp->scaleStep && cur.transparent == bmp->transparent;
		if( bmp && !sameAsCur )
		{
			if( !cur.valid || cur.page != page )
			{
				// A different page: cross-fade from what is on screen now.
				std::swap( cur, prev );
				fading    = transition > 0.001f;
				fadeStart = clock::now();
			}
			Upload( cur, *bmp, ps );
		}
	}

	float fade = 1.0f;
	if( fading )
	{
		float t = std::chrono::duration< float >( clock::now() - fadeStart ).count();
		fade    = std::min( 1.0f, t / std::max( 0.001f, transition ) );
		if( fade >= 1.0f )
			fading = false;
	}
	// Smoothstep feels nicer than a linear dissolve on text.
	fade = fade * fade * ( 3.0f - 2.0f * fade );

	const bool showCur  = cur.valid && page >= 0;
	const bool showPrev = fading && prev.valid;

	Rect rc = showCur ? ComputeRect( cur.size ) : Rect{ 0, 0, 1, 1, 1 };
	Rect rp = showPrev ? ComputeRect( prev.size ) : Rect{ 0, 0, 1, 1, 1 };

	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, showPrev ? prev.tex : 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, showCur ? cur.tex : 0 );

	glUniform1i( shader.FindUniform( "curTex" ), 0 );
	glUniform1i( shader.FindUniform( "prevTex" ), 1 );
	glUniform2f( shader.FindUniform( "resolution" ), (float)currentViewport.width, (float)currentViewport.height );
	glUniform4f( shader.FindUniform( "curRect" ), rc.cx, rc.cy, rc.w, rc.h );
	glUniform4f( shader.FindUniform( "prevRect" ), rp.cx, rp.cy, rp.w, rp.h );
	glUniform1i( shader.FindUniform( "hasCur" ), showCur ? 1 : 0 );
	glUniform1i( shader.FindUniform( "hasPrev" ), showPrev ? 1 : 0 );
	glUniform1i( shader.FindUniform( "rotation" ), Rotation() );
	glUniform1f( shader.FindUniform( "fade" ), showPrev ? fade : 1.0f );
	glUniform4f( shader.FindUniform( "background" ), bg[ 0 ], bg[ 1 ], bg[ 2 ], bg[ 3 ] );
	glUniform1i( shader.FindUniform( "invert" ), invert ? 1 : 0 );

	quad.Draw();

	// Leave the context in its default state, as FFGL requires.
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );

	return FF_SUCCESS;
}

FFResult FFGLPdfViewer::SetFloatParameter( unsigned int index, float value )
{
	const bool pressed = value > 0.5f;
	switch( index )
	{
	case PT_RELOAD:
		if( pressed )
			renderer->Reload();
		break;
	case PT_AUTORELOAD:
		autoReload = pressed;
		renderer->SetAutoReload( autoReload );
		break;
	case PT_PAGE: pageValue = value; break;
	case PT_PREV:
		if( pressed )
			GoToPage( (int)std::lround( pageValue ) - 1 );
		break;
	case PT_NEXT:
		if( pressed )
			GoToPage( (int)std::lround( pageValue ) + 1 );
		break;
	case PT_FIRST:
		if( pressed )
			GoToPage( 0 );
		break;
	case PT_LAST:
		if( pressed )
			GoToPage( pageCount - 1 );
		break;
	case PT_TRANSITION: transition = std::max( 0.0f, value ); break;
	case PT_FIT: fitMode = (int)std::lround( value ); break;
	case PT_ZOOM: zoom = std::max( 0.01f, value ); break;
	case PT_PANX: panX = value; break;
	case PT_PANY: panY = value; break;
	case PT_ROTATE: rotation = (int)std::lround( value ); break;
	case PT_QUALITY: qualityIndex = (int)std::lround( value ); break;
	case PT_PAGEBG: pageBackground = (int)std::lround( value ); break;
	case PT_INVERT: invert = pressed; break;
	case PT_BG_RED: bg[ 0 ] = value; break;
	case PT_BG_GREEN: bg[ 1 ] = value; break;
	case PT_BG_BLUE: bg[ 2 ] = value; break;
	case PT_BG_ALPHA: bg[ 3 ] = value; break;
	default: return FF_FAIL;
	}
	return FF_SUCCESS;
}

float FFGLPdfViewer::GetFloatParameter( unsigned int index )
{
	switch( index )
	{
	case PT_AUTORELOAD: return autoReload ? 1.0f : 0.0f;
	case PT_PAGE: return pageValue;
	case PT_TRANSITION: return transition;
	case PT_FIT: return (float)fitMode;
	case PT_ZOOM: return zoom;
	case PT_PANX: return panX;
	case PT_PANY: return panY;
	case PT_ROTATE: return (float)rotation;
	case PT_QUALITY: return (float)qualityIndex;
	case PT_PAGEBG: return (float)pageBackground;
	case PT_INVERT: return invert ? 1.0f : 0.0f;
	case PT_BG_RED: return bg[ 0 ];
	case PT_BG_GREEN: return bg[ 1 ];
	case PT_BG_BLUE: return bg[ 2 ];
	case PT_BG_ALPHA: return bg[ 3 ];
	default: return 0.0f;// events
	}
}

FFResult FFGLPdfViewer::SetTextParameter( unsigned int index, const char* value )
{
	std::string v = value ? value : "";
	switch( index )
	{
	case PT_FILE:
		if( v != filePath )
		{
			filePath  = v;
			fileDirty = true;
		}
		break;
	case PT_PASSWORD:
		if( v != password )
		{
			password  = v;
			fileDirty = true;
		}
		break;
	default: return FF_FAIL;
	}
	return FF_SUCCESS;
}

char* FFGLPdfViewer::GetTextParameter( unsigned int index )
{
	switch( index )
	{
	case PT_FILE: return const_cast< char* >( filePath.c_str() );
	case PT_PASSWORD: return const_cast< char* >( password.c_str() );
	default: return nullptr;
	}
}

char* FFGLPdfViewer::GetParameterDisplay( unsigned int index )
{
	switch( index )
	{
	case PT_ZOOM: std::snprintf( displayBuffer, sizeof( displayBuffer ), "%.2fx", zoom ); return displayBuffer;
	case PT_TRANSITION: std::snprintf( displayBuffer, sizeof( displayBuffer ), "%.2f s", transition ); return displayBuffer;
	case PT_PANX:
	case PT_PANY:
		std::snprintf( displayBuffer, sizeof( displayBuffer ), "%.0f%%", ( index == PT_PANX ? panX : panY ) * 100.0f );
		return displayBuffer;
	default: return CFFGLPlugin::GetParameterDisplay( index );
	}
}
