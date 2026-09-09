#pragma once

#include <wincodec.h>
#include <wincodecsdk.h>
#pragma comment(lib,"WindowsCodecs.lib")


#include "wic_loader.h"
#include <vector>

#include <Icm.h>
//#pragma comment(lib,"icm32.lib")
#pragma comment(lib,"mscms.lib")
namespace ICM
{
static std::wstring GetColorDirectory()
{
	class path
	{
		wchar_t buf[ MAX_PATH ];
	public:
		path()
		{
			DWORD size = MAX_PATH;
			::GetColorDirectoryW(NULL, buf, &size );
		}

		const wchar_t*const get()const
		{
			return buf;
		}
	};
	static path r  = path();
	return r.get();
}

static void  GetsRGBProfile(LPCBYTE& Buffer, DWORD& Size)
{
class profile
	{
		std::vector<BYTE> buf;;
	public:
		profile()
		{
			std::wstring path = ICM::GetColorDirectory();
			path +=  L"\\sRGB Color Space Profile.icm";
	
			HANDLE  file = CreateFileW( path.c_str(), FILE_GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
			if( file != INVALID_HANDLE_VALUE)
			{
				LARGE_INTEGER file_size={};
				if( GetFileSizeEx( file, &file_size ) )
				{
					if( file_size.LowPart > 0 && file_size.HighPart == 0  )
					{
						DWORD size = file_size.LowPart;
						buf.resize(size);
						DWORD read;
						if( !ReadFile( file, &buf[0],size, &read, 0 ) )
						{
							DWORD error = GetLastError() ;
						}
					}
				}
				CloseHandle( file );	
			}
			
		}

		const BYTE* const get()const
		{
			return buf.data();
		}
		const DWORD  size()const
		{
			return buf.size();
		}
	};
	static profile r  = profile();
Buffer= r.get();
Size= r.size();
};
static void  GetDisplayProfile(LPCBYTE& Buffer, DWORD& Size)
{
class profile
	{
		std::vector<BYTE> buf;;
	public:
		profile()
		{
			wchar_t ICC_path[MAX_PATH]={};
			HDC hdc = GetDC(0);
			DWORD size = MAX_PATH;
			BOOL ret = GetICMProfileW( hdc, &size, ICC_path );
			ReleaseDC( 0,hdc);
			if( ret )
			{
				HANDLE  file = CreateFileW( ICC_path, FILE_GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
				if( file != INVALID_HANDLE_VALUE)
				{
					LARGE_INTEGER file_size={};
					if( GetFileSizeEx( file, &file_size ) )
					{
						if( file_size.LowPart > 0 && file_size.HighPart == 0  )
						{
							DWORD size = file_size.LowPart;
							buf.resize(size);
							DWORD read;
							if( !ReadFile( file, &buf[0],size, &read, 0 ) )
							{
								DWORD error = GetLastError() ;
							}
						}
					}
					CloseHandle( file );
				}		
			}
			else
			{
				LPCBYTE b;
				DWORD s;
				GetsRGBProfile( b, s );
				buf.assign( b, b+s );
			}
		}

		const BYTE*const get()const
		{
			return buf.data();
		}
		const DWORD  size()const
		{
			return buf.size();
		}
	};
	static profile r  = profile();
Buffer= r.get();
Size= r.size();
};
}//ICM
class WicLoader
{
public:
	WicLoader(void);
	~WicLoader(void);

	enum ExifColorSpace
	{
		sRGB = 1,
		AdobeRGB = 2
	};
private:
	com_ptr<IWICImagingFactory> m_factory;

	com_ptr<IWICBitmapDecoder> m_decoder;
	com_ptr<IWICBitmapFrameDecode> m_frame;
	com_ptr<IWICColorContext> m_srcColorContext;
	com_ptr<IWICColorContext> m_dstColorContext;
	com_ptr< IWICColorTransform > m_dstColorTransform;
	com_ptr<IWICBitmapSource> m_originalBitmap;
	com_ptr<IWICBitmapSource> m_colorTranslatedBitmap;
	com_ptr<IWICBitmapCodecProgressNotification > m_ProgressNotification;

	WICPixelFormatGUID m_frameFormat;

	bool m_colorManagement;
	int m_width;
	int m_height;
	UINT m_frames;
	int m_colorDepth;

	bool m_over8bpp;
	bool m_hasAlpha;
	

	enum ColorSpace
	{
		ColorSpaceUnknown,
		ColorSpacesRGB,
		ColorSpacescRGB
	};
	ColorSpace m_dstColorSpace;
	//ICM::ColorTranslate m_ColorTranslate;

	std::vector<BYTE> m_InputColorProfileData;


	void loadColorProfile(ExifColorSpace value);
	HRESULT loadColorProfile( );

	HRESULT getFormat();
	HRESULT convert(const WICPixelFormatGUID& DstFormat );

public:
	HRESULT Open(HANDLE FileHandle );
	HRESULT Open(IStream* stream);
	HRESULT Open(LPCWSTR FileName);

	HRESULT Decode(void);
	HRESULT GetInfomation(PictureInfo& info);
	HRESULT WriteToBuffer(LPBYTE DstBuffer, DWORD DstSize, LONG DstStride );
	HRESULT RegisterProgressNotification(   __in_opt  PFNProgressNotification pfnProgressNotification,
            /* [annotation][unique][in] */ 
            __in_opt  LPVOID pvData,
            /* [in] */ DWORD dwProgressFlags)
	{
		if(m_ProgressNotification)
		{
			return m_ProgressNotification->RegisterProgressNotification(pfnProgressNotification, pvData, dwProgressFlags );
		}
		return S_OK;
	}

	
int GetWidth()const
{
	return m_width;
}
int GetHeight()const
{
	return m_height;
}
UINT GetFrameCount()const
{ 
	return m_frames; 
}
};
