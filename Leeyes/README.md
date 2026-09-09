# Leeyes/ (ローカル動作確認用・git管理外)

ビルド後のポストビルドイベントが `inject_dll.dll` / `LeeyesW.exe` / `WIC_Loader.spi` を
このフォルダにコピーします。

ここに、別途入手した本物の Leeyes 本体一式(Leeyes.exe, 7z.dll, ax7z.spi 等の
Susieプラグイン)を配置すると、そのままこのフォルダ内で動作確認できます。

これらの third-party バイナリはライセンス上 git には含めていません
(`.gitignore` で `/Leeyes/` 以下は除外済み、このREADME自体だけは残しています)。
