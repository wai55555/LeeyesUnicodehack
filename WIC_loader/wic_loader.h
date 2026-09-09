#pragma once

#pragma pack(push)
#pragma pack(1) 
struct PictureInfo
{
	long left;
	long top;
	long width;
	long height;
	WORD x_density;		/* dpi */
	WORD y_density;		/* dpi */
	short colorDepth;
	HLOCAL hInfo;		/* 画像内のテキスト情報 */
};
#pragma pack(pop)


enum  SusieErrorCode :int
{
SPI_NO_FUNCTION	=	-1	,	/* その機能はインプリメントされていない */
SPI_ALL_RIGHT,					/* 正常終了 */
SPI_ABORT	,						/* コールバック関数が非0を返したので展開を中止した */
SPI_NOT_SUPPORT	,			/* 未知のフォーマット */
SPI_OUT_OF_ORDER,			/* データが壊れている */
SPI_NO_MEMORY,				/* メモリーが確保出来ない */
SPI_MEMORY_ERROR,		/* メモリーエラー */
SPI_FILE_READ_ERROR,		/* ファイルリードエラー */
SPI_WINDOW_ERROR	,		/* 窓が開けない (非公開のエラーコード) */
SPI_OTHER_ERROR	,			/* 内部エラー */
SPI_FILE_WRITE_ERROR,	/* 書き込みエラー (非公開のエラーコード) */
SPI_END_OF_FILE,				/* ファイル終端 (非公開のエラーコード) */
};

typedef int (CALLBACK *SPI_PROGRESS)(int, int, long);
	int __declspec(dllexport) __stdcall GetPluginInfo
			(int infono, LPSTR buf, int buflen);
	int __declspec(dllexport) __stdcall IsSupported(LPSTR filename, DWORD dw);
	int __declspec(dllexport) __stdcall GetPictureInfo
			(LPSTR buf,long len, unsigned int flag, PictureInfo *lpInfo);
	int __declspec(dllexport) __stdcall GetPicture
			(LPSTR buf,long len, unsigned int flag,
			 HANDLE *pHBInfo, HANDLE *pHBm,
			 SPI_PROGRESS lpPrgressCallback, long lData);
	int __declspec(dllexport) __stdcall GetPreview
			(LPSTR buf,long len, unsigned int flag,
			 HANDLE *pHBInfo, HANDLE *pHBm,
			 SPI_PROGRESS lpPrgressCallback, long lData);


static LPCSTR pluginfo[] = 
{
	"00IN",	/* Plug-in API バージョン */
	"WIC UnicodeHack",	/* Plug-in名,バージョン及び copyright */
	"*.bmp;*.DIB;*.GIF;*.JPG;*.JPEG;*.PNG;*.TIF;*.TIFF;*.hdp;*.wdp",	/* 代表的な拡張子 ("*.JPG" "*.JPG;*.JPEG" など) */
	"BMP file(*.BMP);GIF(*.GIF);JPEG(*.JPG);PNG(*.PNG)" ,	/* ファイル形式名 */
};
