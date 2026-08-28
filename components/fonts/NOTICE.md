# フォントの出典とライセンス

`lw_font_jp_16.c` と `lw_font_jp_24.c` は、**Noto Sans JP Regular** から
このアプリが実際に表示する文字だけを抜き出して生成したビットマップフォントです。

- 元フォント: [Noto Sans JP](https://github.com/notofonts/noto-cjk) (Google)
- ライセンス: **SIL Open Font License, Version 1.1**
- ライセンス全文: [`LICENSE-NotoSansJP.txt`](LICENSE-NotoSansJP.txt)

生成物は元フォントの派生物にあたるため、OFL の条件が及びます。
再配布する場合はこの告知とライセンス全文を必ず同梱してください。

なお OFL は派生フォントに元の名前を使うことを禁じているため、
生成物の名前は `lw_font_jp_*` としており `Noto` を含めていません。

## 再生成

文言を変更したら [`generate.sh`](generate.sh) を実行して作り直します。
文字集合は [`ui_strings.txt`](ui_strings.txt) から自動抽出されるので、
文字リストを手で管理する必要はありません。

```sh
./components/fonts/generate.sh
```

必要なもの: node/npm（`npx lv_font_conv` 用）と、初回のみ curl。
元の OTF ファイルは `.gitignore` で除外されており、スクリプトが自動で取得します。
