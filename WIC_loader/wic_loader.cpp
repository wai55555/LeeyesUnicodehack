#include "wic_loader.h"
#include "WicLoader.h"
#include <agents.h>
#include <clocale>

/* �G���g���|�C���g */
BOOL APIENTRY DllMain(HANDLE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH: 
		::setlocale(LC_ALL, "Japanese_Japan.932");
		break;
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

int __stdcall GetPluginInfo(int infono, LPSTR buf, int buflen)
{
	if (infono < 0 || infono >= (sizeof(pluginfo) / sizeof(char *))) 
	{
		return 0;
	}

	::lstrcpynA(buf, pluginfo[infono], buflen);

	return ::lstrlenA(buf);
}

int __stdcall IsSupported(LPSTR filename, DWORD dw)
{
	return 1;
}

int __stdcall GetPictureInfo(LPSTR buf, long len, unsigned int flag, struct PictureInfo *lpInfo)
{
	return  SPI_NO_FUNCTION;
}

std::wstring GetWidePath( std::string Path )
{
	std::wstring settled_path ;
	std::string tmp;
	wchar_t wide_file_name[MAX_PATH]={};
	bool unknown_code_found = false;

	tmp .reserve( Path.size() );
	settled_path .reserve( MAX_PATH );
	for( auto itr = Path.begin(); itr != Path.end(); ++itr )
	{
		if(*itr == '?' ){ unknown_code_found = true; }
		if( isleadbyte( static_cast<BYTE>(*itr) ) )
		{
			tmp.push_back( *itr );
			++itr;
			tmp.push_back( *itr );
			continue;
		}
		else
		{
			if( *itr < 0 )
			{
					tmp.push_back( '?');
			}
			else
			{
					tmp.push_back( *itr );
			}
		}
		if( *itr == '\\' )
		{
			if( unknown_code_found )
			{//�f�B���N�g���������j�R�[�h
				if( ::MultiByteToWideChar(932, 0, tmp.data(), tmp.size()+1 , wide_file_name, MAX_PATH ) )
				{
					std::wstring indefinite_path( settled_path  );
					indefinite_path.append(wide_file_name );
					indefinite_path.pop_back();
					indefinite_path.push_back(L'.');

					_WIN32_FIND_DATAW FindData={};
					HANDLE hFind = ::FindFirstFileW(indefinite_path.c_str() ,&FindData);
					if( hFind != INVALID_HANDLE_VALUE )
					{
						do
						{
							if( FindData.cFileName[0] != L'.' )//?��������[.],[..]�Ƃ������|����
							{
								settled_path .append( FindData.cFileName );
								settled_path .push_back(L'\\' );
								break;
							}
						}	while( ::FindNextFileW( hFind, &FindData ) );
						::FindClose( hFind );
					}
				}
				unknown_code_found = false;
			}
			else
			{
				if( ::MultiByteToWideChar(932, 0, tmp.data(), tmp.size()+1 , wide_file_name, MAX_PATH ) ) 
				{
					settled_path .append( wide_file_name );
				}
			}
			tmp.clear();
		}
	}

	if( unknown_code_found )
	{//�t�@�C���������j�R�[�h
		if(::MultiByteToWideChar(932, 0, tmp.data(), tmp.size()+1 , wide_file_name, MAX_PATH ))
		{
			std::wstring indefinite_path( settled_path  );
			indefinite_path.append(wide_file_name );
			_WIN32_FIND_DATAW FindData={};
			HANDLE hFind = FindFirstFileW(indefinite_path.c_str() ,&FindData);
			if( hFind != INVALID_HANDLE_VALUE )
			{			
				settled_path .append( FindData.cFileName );
				FindClose( hFind );
			}
			else
			{//�T���Q�[�g�y�A�Ƃ����Ƃ���������?
				//short name
			}
		}
		unknown_code_found = false;
	}
	else
	{
		if( ::MultiByteToWideChar(932, 0, tmp.data(), tmp.size()+1 , wide_file_name, MAX_PATH ) )
		{
			settled_path .append( wide_file_name   );
		}
	}	
	return settled_path ;
}


struct SPI_PROGRESS_DATA
{
	SPI_PROGRESS lpPrgressCallback;
	long lData;
};

HRESULT __stdcall Progress(  LPVOID pvData, ULONG uFrameNum,  WICProgressOperation operation,  double dblProgress)
{
	SPI_PROGRESS_DATA* spi( reinterpret_cast<SPI_PROGRESS_DATA*>(pvData) );
	if( spi && spi->lpPrgressCallback)
	{
		int n = static_cast<int>(100*dblProgress);
		if( spi->lpPrgressCallback( n,100, spi->lData ) )
		{
			return WINCODEC_ERR_ABORTED ;
		}
	}
	return S_OK;
}
	
class wic : public Concurrency::agent
{
	HANDLE m_HBInfo;
	HANDLE m_HBm;
	int m_ret;
	 LPSTR buf;
	 long len;
	 unsigned int flag;
	 SPI_PROGRESS_DATA spi;
	 DWORD m_thread;
public:
	explicit wic( LPSTR buf, long len, unsigned int flag ,SPI_PROGRESS_DATA spi)
		:m_HBInfo(0),m_HBm(0),m_ret(0),buf(buf),len(len),flag(flag),spi(spi),m_thread(0)
	{
	}
	int GetReturnCode()const{ return m_ret; }
	int Get( HANDLE* pHBInfo, HANDLE* pHBm )const
	{
		*pHBInfo = m_HBInfo;
		*pHBm = m_HBm;
		return m_ret;
	}
	DWORD GetThreadID()const{ return m_thread; }
protected:
   void run()
   {
	   m_thread = GetCurrentThreadId();
	   
		auto  loader = std::shared_ptr<WicLoader>( new WicLoader() );
		HRESULT hr = E_FAIL;
		if ((flag & 7) == 0) 
		{
			hr =loader->Open( GetWidePath(buf).c_str() );
			if( FAILED(hr) )
			{
				m_ret = SPI_FILE_READ_ERROR;done();
					return ;
			}
		} 
		else 
		{
			com_ptr<IStream> stream (SHCreateMemStream((const BYTE*)buf, len) );
			if( stream == 0 )
			{
				m_ret =SPI_FILE_READ_ERROR;done();
				return ;
			}
			hr = loader->Open( stream );
		
			if( FAILED(hr) )
			{
				m_ret =SPI_FILE_READ_ERROR;done();
					return ;
			}

		}
		if(SUCCEEDED(hr))
		{

			hr = loader->RegisterProgressNotification( &Progress, &spi, WICProgressOperationAll);
		}
		if(SUCCEEDED(hr))
		{
			hr = loader->Decode();
			if(FAILED(hr))
			{
				m_ret =SPI_ABORT;done();
				return ;
		
			}
		}
		HANDLE HBInfo(NULL);
		HANDLE HBm(NULL);

		if(SUCCEEDED(hr))
		{
			HBInfo = ::LocalAlloc(LMEM_FIXED, sizeof(BITMAPINFO)  );
			if( HBInfo == NULL )
			{
				m_ret =SPI_NO_MEMORY;done();
				return ; 
			}
			DWORD bmp_stride_byte =  ((24/8) *loader->GetWidth() + 3) & ~3;
			DWORD bmp_image_size =  loader->GetHeight() * bmp_stride_byte;
			HBm = ::LocalAlloc(LMEM_FIXED, bmp_image_size  );
			if( HBm == NULL )
			{
				::LocalFree(HBInfo);
				m_ret =SPI_NO_MEMORY;done();
				return ; 
			}
			BITMAPINFO *bitmapinfo = (BITMAPINFO *)::LocalLock(HBInfo);
			if( !bitmapinfo )
			{
				::LocalFree(HBInfo);
				::LocalFree(HBm);
				m_ret =SPI_MEMORY_ERROR;done();
				return ;
			}

			LPBYTE bmp_data = (LPBYTE)(::LocalLock( HBm));
			if( !bmp_data )
			{
				::LocalUnlock( HBInfo );
				::LocalFree(HBInfo);
				::LocalFree(HBm);
				m_ret = SPI_MEMORY_ERROR;done();
				return ;
			}
			{

			memset( bitmapinfo, 0, sizeof( BITMAPINFO ) );
			BITMAPINFOHEADER *header  = &bitmapinfo->bmiHeader;
			header->biSize = sizeof( BITMAPINFO );
			header->biHeight = loader->GetHeight();
			header->biWidth = loader->GetWidth();
			header->biPlanes			= 1;
			header->biBitCount = 24;
			header->biSizeImage = 0;

			::LocalUnlock( HBInfo );

			if(SUCCEEDED( loader->WriteToBuffer( bmp_data, bmp_image_size, bmp_stride_byte ) ) )
			{
				::LocalUnlock(HBm);
				if (spi.lpPrgressCallback != NULL)
				{
					if (spi.lpPrgressCallback(1, 1, spi.lData)) /* 0% */
					{
						m_ret =SPI_ABORT;done();
						return ;
					}
				}
				m_HBm = HBm;
				m_HBInfo = HBInfo;
				m_ret =SPI_ALL_RIGHT;
				done();
				return ;
			}
			::LocalUnlock( HBm);
			}
		}
	  done();
   }
};


// �߂�l: ���� �]�݂�HWND / ���s NULL
HWND GetWindowHandle(	const DWORD TargetID)	// �v���Z�XID
{
	HWND hwnd = ::GetTopWindow(NULL);
	do 
	{
		if( ::GetWindowLong( hwnd, GWL_HWNDPARENT) != 0 || !::IsWindowVisible( hwnd))
		{
			continue;
		}
		DWORD process_id(0);
		::GetWindowThreadProcessId( hwnd, &process_id);
		if(TargetID == process_id)
		{
			return hwnd;
		}
	} while( (hwnd =::GetNextWindow( hwnd, GW_HWNDNEXT)) != NULL);

	return NULL;
}

int __stdcall GetPicture(	LPSTR buf, long len, unsigned int flag, HANDLE *pHBInfo, HANDLE *pHBm,
						 SPI_PROGRESS lpPrgressCallback, long lData)
{
	int ret(SPI_OTHER_ERROR);
	SPI_PROGRESS_DATA spi;
	spi.lData = lData;
	spi.lpPrgressCallback = lpPrgressCallback;
	if (lpPrgressCallback != NULL)
	{
		if (lpPrgressCallback(0, 1, lData)) /* 0% */
		{
			return SPI_ABORT;
		}
	}

	//�t���[�Y����Ǝ肪�o���Ȃ�����ʃv���Z�X�ɂ���̂��ǂ��񂾂낤���ǁc
	wic w(buf,len,flag,spi);
	w.start();
	try
	{
			concurrency::agent::wait( &w, 1000*15 );
	}
	catch( concurrency::operation_timed_out& /*timedout*/ )
	{
		auto handle = ::OpenThread(THREAD_ALL_ACCESS, FALSE, w.GetThreadID() );
		::SuspendThread(handle);
		::CloseHandle( handle );
		auto process  = ::GetCurrentProcessId();
		auto hwnd = ::GetWindowHandle( process );
		auto ret = ::MessageBoxW( hwnd, L"�s���S�ȃt�@�C���ɂ��n���O�A�b�v���Ă�̂ōċN���𐄏�\n���~�{�^���ŏI�������܂�\n���̑��̃{�^���͕s����Ȃ܂ܑ��s���܂�",L"WIC Loader", MB_ABORTRETRYIGNORE);
		if( ret == IDABORT)
		{
			::ExitProcess( -1 ) ;
		}
		throw;
	}
	catch( std::exception& e )
	{
		e.what();
		return SPI_OTHER_ERROR;
	}

	switch (w.status())
	{
	case concurrency::agent_canceled:
		return SPI_ABORT;
		break;
	case concurrency::agent_done:
		return w.Get( pHBInfo, pHBm );
	default:
		break;
	}
	return ret;
}

int __stdcall GetPreview(LPSTR buf, long len, unsigned int flag, HANDLE *pHBInfo, HANDLE *pHBm, SPI_PROGRESS lpPrgressCallback, long lData)
{
	return SPI_NO_FUNCTION;
	//Leeyes�͎g�p����̂ł܂Ƃ��Ɏ�������ׂ�
	//XP�؂�̂Ă�IThumbnailCache����������WIC���̂܂܂Ɠ����Ή��t�@�C��
}
