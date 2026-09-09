#include "WicLoader.h"
#include <thread>
#include <stdexcept>

WicLoader::WicLoader(void):m_colorManagement(false),m_hasAlpha(false),m_colorDepth(0),m_over8bpp(),m_frameFormat(),m_frames(),m_width(0),m_height(0)
{	
	CoInitialize(0);

	m_dstColorSpace=ColorSpaceUnknown;
	HRESULT hr =CoCreateInstance(
		CLSID_WICImagingFactory,
		NULL,
		CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(m_factory.ToCreator() ) 
		);

}

WicLoader::~WicLoader(void)
{
	CoUninitialize();
}


void WicLoader::loadColorProfile(ExifColorSpace value)
{
	if(FAILED( m_srcColorContext->InitializeFromExifColorSpace( value ) ) )
	{
		throw std::runtime_error("InitializeFromExifColorSpace");
	}
	return;
	if( value == WicLoader::sRGB )
	{
			LPCBYTE buf;
			DWORD size;
			ICM::GetsRGBProfile( buf, size );
			m_InputColorProfileData.assign(buf, buf+size );
			return;
	}


	std::wstring path = ICM::GetColorDirectory();
	//	path +=  L"\\scRGB.icm";

	switch (value)
	{
	case WicLoader::sRGB:
		
		path +=  L"\\sRGB Color Space Profile.icm";
		break;
	case WicLoader::AdobeRGB:
		path +=  L"\\AdobeRGB.icm";
		break;
	default:
		throw std::runtime_error("InitializeFromExifColorSpace");
		break;
	}
	
	HANDLE  file = CreateFileW( path.c_str(), FILE_GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
	if( file != INVALID_HANDLE_VALUE)
	{
		LARGE_INTEGER file_size={};
		if( GetFileSizeEx( file, &file_size ) )
		{
			if( file_size.LowPart > 0 && file_size.HighPart == 0  )
			{
				DWORD size = file_size.LowPart;
				m_InputColorProfileData.resize(size);
				DWORD read;
				if( !ReadFile( file, &m_InputColorProfileData[0],size, &read, 0 ) )
				{
					DWORD error = GetLastError() ;
				}
			}
		}
		CloseHandle( file );
	}
}

HRESULT WicLoader::loadColorProfile( )
{

	HRESULT ret= S_OK;
	IWICColorContext* source_color_context_tmp;
	UINT count=0;
	HRESULT hr = m_factory->CreateColorContext( &source_color_context_tmp );
	if( SUCCEEDED(hr) )
	{	
		hr = m_frame->GetColorContexts(0, NULL, &count );
		hr = m_frame->GetColorContexts(1, &source_color_context_tmp, &count );
		m_srcColorContext.reset(source_color_context_tmp);
		if( SUCCEEDED(hr) && count ==1 )
		{
			UINT size = 0;

			hr = m_srcColorContext->GetProfileBytes(0,NULL,&size);
			if( SUCCEEDED(hr) )
			{
				m_InputColorProfileData.resize(size);
				hr = m_srcColorContext->GetProfileBytes(
					static_cast<UINT>(m_InputColorProfileData.size()),
					&m_InputColorProfileData[0],&size);
			}
			return hr;
		}
		else
		{
			com_ptr<IWICMetadataQueryReader> metadata_query_reader;
			hr = m_frame->GetMetadataQueryReader( metadata_query_reader.ToCreator() );
			if( SUCCEEDED(hr) )
			{
				
				// EXIF�^�O�̏ꏊ���Ⴄ�\��������H�H
				PROPVARIANT v;
				PropVariantInit(&v);
				if(FAILED( hr = metadata_query_reader->GetMetadataByName(L"/app1/ifd/exif/subifd:{uint=40961}", &v) ) )
				{
					if( FAILED(hr = metadata_query_reader->GetMetadataByName(L"/ifd/exif/subifd:{uint=40961}", &v)))
					{
						loadColorProfile(sRGB);
						return hr;
					}
				}
  
				UINT cs = (v.vt == VT_UI2) ? v.uiVal : 0;
				PropVariantClear(&v);

				// 1,sRGB 2,Adobe RGB
				if( cs == sRGB || cs == AdobeRGB )
				{
					loadColorProfile(static_cast<ExifColorSpace>(cs));
					return hr;
				}
			}
		}
	}
	else
	{
		throw std::runtime_error("CreateColorContext");
	}
	loadColorProfile(sRGB);
	return hr;
}


HRESULT WicLoader::Open(HANDLE FileHandle )
{
	HRESULT hr =   m_factory->CreateDecoderFromFileHandle( (ULONG_PTR)FileHandle , 
			NULL,                            // Do not prefer a particular vendor
            WICDecodeMetadataCacheOnDemand,  // Cache metadata when needed
			m_decoder.ToCreator()                     // Pointer to the decoder
            );
	
	if( SUCCEEDED(hr) )
	{
		hr = m_decoder->QueryInterface( m_ProgressNotification.ToCreator() );
		if( hr == E_NOINTERFACE ) hr= S_OK;
	}
	return hr;
}
HRESULT  WicLoader::Open(IStream* stream)
{
	HRESULT hr =   m_factory->CreateDecoderFromStream( stream,
			NULL,                            // Do not prefer a particular vendor
            WICDecodeMetadataCacheOnDemand,  // Cache metadata when needed
			m_decoder.ToCreator()                     // Pointer to the decoder
            );

	
	if( SUCCEEDED(hr) )
	{
		hr = m_decoder->QueryInterface( m_ProgressNotification.ToCreator() );
		if( hr == E_NOINTERFACE ) hr= S_OK;
	}
	return hr;
}

HRESULT WicLoader::Open(LPCWSTR FileName)
{
	HRESULT hr = 

	 m_factory->CreateDecoderFromFilename(
            FileName,                      // Image to be decoded
            NULL,                            // Do not prefer a particular vendor
            GENERIC_READ,                    // Desired read access to the file
            WICDecodeMetadataCacheOnDemand,  // Cache metadata when needed
			m_decoder.ToCreator()                     // Pointer to the decoder
            );
	
	
	if( SUCCEEDED(hr) )
	{
		hr = m_decoder->QueryInterface( m_ProgressNotification.ToCreator() );
		if( hr == E_NOINTERFACE ) hr= S_OK;
	}
	return hr;
}



HRESULT WicLoader::Decode(void)
{
	HRESULT hr = S_OK;
	hr = m_decoder->GetFrameCount(&m_frames);
	if (SUCCEEDED(hr))
	{
		hr = m_decoder->GetFrame(0, m_frame.ToCreator() );
	}

	if (SUCCEEDED(hr))
	{
		hr = m_frame->QueryInterface(IID_PPV_ARGS(m_originalBitmap.ToCreator()) );         
	}
	if (SUCCEEDED(hr))
	{
		UINT w,h;
		m_originalBitmap->GetSize( &w, &h );
		m_width = w;
		m_height = h;
		hr = getFormat();
	}



	m_colorManagement	 = true;
	if( m_colorManagement )
	{
		if (SUCCEEDED(hr))
		{
			hr = m_factory->CreateColorContext( m_srcColorContext.ToCreator() );
		}	
		hr = loadColorProfile();

		if(  m_dstColorSpace != ColorSpace::ColorSpacesRGB )
		{
	
			hr = m_factory->CreateColorContext( m_dstColorContext.ToCreator() );
			if( SUCCEEDED(hr) )
			{
				if(0)
				{//�W����Ԓʂ����ɒ��ڃ��C�����j�^�̏o�̓v���t�@�C���ɕϊ��ACMS��Ή��A�v�����Ǝd���Ȃ�
						LPCBYTE buf;
						DWORD size;
						ICM::GetDisplayProfile( buf, size );
						hr = m_dstColorContext->InitializeFromMemory(buf,size);
				}
				else
				{
					hr = m_dstColorContext->InitializeFromExifColorSpace( sRGB );
				}
			}
			
			if( SUCCEEDED(hr) )
			{
					m_dstColorSpace = ColorSpace::ColorSpacesRGB;
			}
			if( SUCCEEDED(hr) )
			{
					hr =	m_factory->CreateColorTransformer( m_dstColorTransform.ToCreator() );
			}
			if( SUCCEEDED(hr) )
			{
				hr = m_dstColorTransform->Initialize( m_originalBitmap.get(), m_srcColorContext.get(), m_dstColorContext.get(), GUID_WICPixelFormat24bppBGR ) ;
			}
			if( SUCCEEDED(hr) )
			{
				hr = m_dstColorTransform->QueryInterface( IID_PPV_ARGS( m_originalBitmap.ToCreator()));
			}
			
		}
		if( FAILED(hr)  )
		{
				hr = convert( GUID_WICPixelFormat24bppBGR );	
		}
	}
	else
	{
			hr = convert( GUID_WICPixelFormat24bppBGR );	
	
	}



	return hr;
}


HRESULT WicLoader::convert(const WICPixelFormatGUID& DstFormat )
{
	com_ptr<IWICFormatConverter> format_converter;
	HRESULT hr = m_factory->CreateFormatConverter( format_converter.ToCreator() );
	
	if (SUCCEEDED(hr))
	{
		hr = format_converter->Initialize(
			m_originalBitmap.get(),				// Input bitmap to convert
			DstFormat,									// Destination pixel format
			WICBitmapDitherTypeNone,		// Specified dither patterm
			nullptr,											// Specify a particular palette 
			0.f,												// Alpha threshold
			WICBitmapPaletteTypeCustom	// Palette translation type
			);
		if (SUCCEEDED(hr))
		{
			hr = format_converter->QueryInterface( IID_PPV_ARGS( m_originalBitmap.ToCreator()));
		}
		
	}
	return hr;
}


HRESULT WicLoader::getFormat()
{
	bool has_alpha = false;
	bool over8bit = false;
	 m_colorDepth=0;
	WICPixelFormatGUID fmt;
	HRESULT hr = m_frame->GetPixelFormat(&fmt);
	if( SUCCEEDED(hr) )
	{
		m_frameFormat = fmt;
		if( fmt == GUID_WICPixelFormat32bppBGRA ){	has_alpha = true; m_colorDepth=32;	}
		else if( fmt == GUID_WICPixelFormat32bppPBGRA ){	has_alpha = true; m_colorDepth=32;	}
		else if( fmt == GUID_WICPixelFormat32bppRGBA ){	has_alpha = true; m_colorDepth=32;	}
		else if( fmt == GUID_WICPixelFormat32bppPRGBA ){	has_alpha = true; m_colorDepth=32;	}
		else if( fmt == GUID_WICPixelFormat64bppRGBA ){	has_alpha = true;	over8bit = true; m_colorDepth=64;	}
		else if( fmt == GUID_WICPixelFormat64bppBGRA ){	has_alpha = true;	over8bit = true; m_colorDepth=64;	}
		else if( fmt == GUID_WICPixelFormat64bppPRGBA ){	has_alpha = true;	over8bit = true;	m_colorDepth=64;}
		else if( fmt == GUID_WICPixelFormat64bppPBGRA ){	has_alpha = true;	over8bit = true; m_colorDepth=64;	}
		else if( fmt == GUID_WICPixelFormat128bppRGBAFloat ){	has_alpha = true;	over8bit = true; m_colorDepth=128;	}
		else if( fmt == GUID_WICPixelFormat128bppPRGBAFloat ){	has_alpha = true;	over8bit = true; m_colorDepth=128;	}
		else if( fmt == GUID_WICPixelFormat64bppRGBAFixedPoint ){	has_alpha = true;	over8bit = true; m_colorDepth=64;	}
		else if( fmt == GUID_WICPixelFormat64bppBGRAFixedPoint ){	has_alpha = true;	over8bit = true; m_colorDepth=64;	}
		else if( fmt == GUID_WICPixelFormat128bppRGBAFixedPoint ){	has_alpha = true;	over8bit = true; m_colorDepth=128;	}
		else if( fmt == GUID_WICPixelFormat128bppRGBFixedPoint ){	has_alpha = true;	over8bit = true; m_colorDepth=128;	}
		else if( fmt == GUID_WICPixelFormat32bppRGBA1010102 ){	has_alpha = true;	over8bit = true; m_colorDepth=32;	}
		else if( fmt == GUID_WICPixelFormat32bppRGBA1010102XR ){	has_alpha = true;	over8bit = true; m_colorDepth=32;	}
		else if( fmt == GUID_WICPixelFormat40bppCMYKAlpha ){	has_alpha = true; m_colorDepth=40;	}
		else if( fmt == GUID_WICPixelFormat80bppCMYKAlpha ){	has_alpha = true;	over8bit = true; m_colorDepth=80;	}
		else if( fmt == GUID_WICPixelFormat32bpp3ChannelsAlpha ){	has_alpha = true; m_colorDepth=32;	}
		else if( fmt == GUID_WICPixelFormat40bpp4ChannelsAlpha ){	has_alpha = true; m_colorDepth=40;	}
		else if( fmt == GUID_WICPixelFormat48bpp5ChannelsAlpha ){	has_alpha = true; m_colorDepth=48;	}
		else if( fmt == GUID_WICPixelFormat56bpp6ChannelsAlpha ){	has_alpha = true; m_colorDepth=56;	}
		else if( fmt == GUID_WICPixelFormat64bpp7ChannelsAlpha ){	has_alpha = true; m_colorDepth=64;	}
		else if( fmt == GUID_WICPixelFormat72bpp8ChannelsAlpha ){	has_alpha = true; m_colorDepth=72;	}
		else if( fmt == GUID_WICPixelFormat64bpp3ChannelsAlpha ){	has_alpha = true;	over8bit = true; m_colorDepth=64;	}
		else if( fmt == GUID_WICPixelFormat80bpp4ChannelsAlpha ){	has_alpha = true;	over8bit = true; m_colorDepth=80;	}
		else if( fmt == GUID_WICPixelFormat96bpp5ChannelsAlpha ){	has_alpha = true;	over8bit = true; m_colorDepth=96;	}
		else if( fmt == GUID_WICPixelFormat112bpp6ChannelsAlpha ){	has_alpha = true;	over8bit = true; m_colorDepth=112;	}
		else if( fmt == GUID_WICPixelFormat128bpp7ChannelsAlpha ){	has_alpha = true;	over8bit = true; m_colorDepth=128;	}
		else if( fmt == GUID_WICPixelFormat144bpp8ChannelsAlpha ){	has_alpha = true;	over8bit = true; m_colorDepth=144;	}
	
		else if( fmt == GUID_WICPixelFormat16bppGray ){	over8bit = true; m_colorDepth=16;	}
		else if( fmt == GUID_WICPixelFormat32bppGrayFloat ){	over8bit = true; m_colorDepth=32;	}
		else if( fmt == GUID_WICPixelFormat48bppRGB ){	over8bit = true; m_colorDepth=48;	}
		else if( fmt == GUID_WICPixelFormat48bppBGR ){	over8bit = true; m_colorDepth=48;	}
		else if( fmt == GUID_WICPixelFormat16bppGrayFixedPoint ){	over8bit = true; m_colorDepth=16;	}
		else if( fmt == GUID_WICPixelFormat32bppBGR101010 ){	over8bit = true; m_colorDepth=32;	}
		else if( fmt == GUID_WICPixelFormat48bppRGBFixedPoint ){	over8bit = true; m_colorDepth=48;	}
		else if( fmt == GUID_WICPixelFormat48bppBGRFixedPoint ){	over8bit = true; m_colorDepth=48;	}
		else if( fmt == GUID_WICPixelFormat96bppRGBFixedPoint ){	over8bit = true; m_colorDepth=96;	}
		else if( fmt == GUID_WICPixelFormat128bppRGBFloat ){	over8bit = true; m_colorDepth=128;	}
		else if( fmt == GUID_WICPixelFormat64bppRGBFixedPoint ){	over8bit = true; m_colorDepth=64;	}
		else if( fmt == GUID_WICPixelFormat128bppRGBFixedPoint ){	over8bit = true; m_colorDepth=128;	}
		else if( fmt == GUID_WICPixelFormat64bppRGBHalf ){	over8bit = true; m_colorDepth=64;	}
		else if( fmt == GUID_WICPixelFormat48bppRGBHalf ){	over8bit = true; m_colorDepth=48;	}
		else if( fmt == GUID_WICPixelFormat32bppRGBE ){	over8bit = true; m_colorDepth=32;	}
		else if( fmt == GUID_WICPixelFormat16bppGrayHalf ){	over8bit = true; m_colorDepth=16;	}
		else if( fmt == GUID_WICPixelFormat32bppGrayFixedPoint ){	over8bit = true; m_colorDepth=32;	}
		else if( fmt == GUID_WICPixelFormat48bpp3Channels ){	over8bit = true; m_colorDepth=48;	}
		else if( fmt == GUID_WICPixelFormat64bpp4Channels ){	over8bit = true; m_colorDepth=64;	}
		else if( fmt == GUID_WICPixelFormat80bpp5Channels ){	over8bit = true; m_colorDepth=80;	}
		else if( fmt == GUID_WICPixelFormat96bpp6Channels ){	over8bit = true; m_colorDepth=96;	}
		else if( fmt == GUID_WICPixelFormat112bpp7Channels ){	over8bit = true; m_colorDepth=112;	}
		else if( fmt == GUID_WICPixelFormat128bpp8Channels ){	over8bit = true; m_colorDepth=128;	}
		else//Color Palette
		{
			com_ptr<IWICPalette> palette;
			hr = m_factory->CreatePalette( palette.ToCreator() );
			if(SUCCEEDED( hr ) )
			{
				hr = m_frame->CopyPalette(palette.get());
				if(SUCCEEDED( hr ) )
				{
					BOOL alpha(FALSE);
					palette->HasAlpha(&alpha);
					has_alpha = alpha!=FALSE;
					UINT  count(0);
					palette->GetColorCount( &count );
					 m_colorDepth=count;
				}	
				hr = S_OK;
			}	
		}
	}
	m_over8bpp = over8bit;
	m_hasAlpha = has_alpha;
	return hr;
}


HRESULT WicLoader::GetInfomation(PictureInfo& info)
{
	HRESULT hr = E_FAIL;
	hr = m_decoder->GetFrameCount(&m_frames);
	if (SUCCEEDED(hr))
	{
		hr = m_decoder->GetFrame(0, m_frame.ToCreator() );
	}
	hr = getFormat();
	if (SUCCEEDED(hr))
	{
		info.colorDepth = m_colorDepth;
	}
	if (SUCCEEDED(hr))
	{
		hr = m_frame->QueryInterface(IID_PPV_ARGS(m_originalBitmap.ToCreator()) );         
	}
	UINT w,h;
	m_originalBitmap->GetSize( &w, &h );
	info.width = m_width = w;
	info.height = m_height = h;
	return hr;
}


HRESULT WicLoader::WriteToBuffer(LPBYTE DstBuffer, DWORD DstSize, LONG DstStride )
{
	HRESULT hr = E_FAIL;
	WICRect rect;
	rect.X = rect.Y = 0;
	rect.Width = m_width;
	rect.Height = m_height;

	USHORT rotate(0);
	com_ptr<IWICMetadataQueryReader> metadata_query_reader;
	hr = m_frame->GetMetadataQueryReader( metadata_query_reader.ToCreator() );
	if( SUCCEEDED(hr) )
	{
		PROPVARIANT v;
		PropVariantInit(&v);
		if(FAILED( hr = metadata_query_reader->GetMetadataByName(L"/app1/ifd/exif/subifd:{uint=274}", &v) ) )
		{
			hr = metadata_query_reader->GetMetadataByName(L"/ifd/exif/subifd:{uint=274}", &v);
		}
		if(SUCCEEDED(hr))
		{
			rotate = (v.vt == VT_UI2) ? v.uiVal:0;
		}
		PropVariantClear(&v);

		{	
			auto r = WICBitmapTransformFlipVertical;
			switch (rotate)
			{
			case 0:
			case 1:
				r = WICBitmapTransformFlipVertical;
				break;
			case 2:
				r =WICBitmapTransformRotate0;
				break;
			case 3:
				r =WICBitmapTransformFlipHorizontal;
				break;
			case 4:
				r=WICBitmapTransformRotate180;
				break;
			case 5:
				r=WICBitmapTransformRotate90;
				break;
			case 6:
				r=WICBitmapTransformRotate90;//need vflip
				break;
			case 7:
				r=WICBitmapTransformRotate270;
				break;
			case 8:
				r=WICBitmapTransformRotate270;//need vflip
				break;

			default:
				break;
			}
	
			if(WICBitmapTransformRotate0 !=r)
			{
				com_ptr< IWICBitmapFlipRotator> flipper;
				hr = m_factory->CreateBitmapFlipRotator( flipper.ToCreator() );	
				if( SUCCEEDED(hr) )
				{
				hr = flipper->Initialize( m_originalBitmap.get(), r );
				}
				if( SUCCEEDED(hr) )
				{	
					hr = flipper->QueryInterface( IID_PPV_ARGS( m_originalBitmap.ToCreator()) );
				}	
			}
		}
		if(rotate == 6 || rotate == 8 )
		{
			com_ptr< IWICBitmapFlipRotator> v_flipper;
			hr = m_factory->CreateBitmapFlipRotator( v_flipper.ToCreator() );
			if( SUCCEEDED(hr) )
			{	
				hr = v_flipper->Initialize( m_originalBitmap.get(), WICBitmapTransformFlipVertical );
			}
			if( SUCCEEDED(hr) )
			{	
				hr = v_flipper->QueryInterface( IID_PPV_ARGS( m_originalBitmap.ToCreator()) );
			}
		}
	}
	else
	{
			com_ptr< IWICBitmapFlipRotator> v_flipper;
			hr = m_factory->CreateBitmapFlipRotator( v_flipper.ToCreator() );
			if( SUCCEEDED(hr) )
			{	
				hr = v_flipper->Initialize( m_originalBitmap.get(), WICBitmapTransformFlipVertical );
			}
			if( SUCCEEDED(hr) )
			{	
				hr = v_flipper->QueryInterface( IID_PPV_ARGS( m_originalBitmap.ToCreator()) );
			}	
	}



	try {
	return m_originalBitmap->CopyPixels( &rect, DstStride, DstSize, DstBuffer );
	}
	catch(...)
	{
		return E_ABORT;
	}
}
