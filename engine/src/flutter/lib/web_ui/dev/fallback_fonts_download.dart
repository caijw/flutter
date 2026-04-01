// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// 字体文件列表下载器。
///
/// 直接调用 getFallbackFontList() 获取字体列表，批量下载 woff2 文件到指定目录。
/// 文件按 URL 路径存储，如:
///   outputDir/notocoloremoji/v32/Yq6P-xxx.woff2
///
/// 用法:
///   dart run dev/fallback_fonts_download.dart [--output=DIR]

import 'dart:io' show Directory, File, exit, Platform, stdout;

import 'package:http/http.dart' as http;
import 'package:path/path.dart' as path;

// ignore: avoid_relative_lib_imports
import '../lib/src/engine/font_fallback_data.dart';
import '../lib/src/engine/noto_font.dart';
import 'environment.dart';

const String expectedUrlPrefix = 'https://fonts.gstatic.com/s/';

void main(List<String> args) async {
  String outputArg = '';
  for (final String a in args) {
    if (a.startsWith('--output=')) {
      outputArg = a.substring('--output='.length);
      break;
    }
  }
  final String outputDir = outputArg.isEmpty
      ? path.join(environment.webUiRootDir.path, 'fallback_fonts')
      : outputArg;

  stdout.writeln('Output directory: $outputDir');

  final List<NotoFont> fallbackFonts = getFallbackFontList();
  stdout.writeln('Found ${fallbackFonts.length} fonts to download.');

  final Directory fontDir = Directory(outputDir);
  if (!fontDir.existsSync()) {
    fontDir.createSync(recursive: true);
  }

  final http.Client client = http.Client();
  int successCount = 0;
  int failCount = 0;

  for (final NotoFont font in fallbackFonts) {
    final String url = '${expectedUrlPrefix}${font.url}';
    final Uri uri = Uri.parse(url);
    final File fontFile = File(path.join(fontDir.path, font.url));

    if (fontFile.existsSync()) {
      stdout.writeln('[skip] ${fontFile.path} family=${font.name} (${fontFile.lengthSync()} bytes)');
      successCount++;
      continue;
    }

    try {
      stdout.writeln('Downloading ${font.name}...');
      final http.Response fontResponse = await client.get(uri);
      if (fontResponse.statusCode != 200) {
        stdout.writeln('[FAIL] ${fontFile.path} family=${font.name} - HTTP ${fontResponse.statusCode}');
        failCount++;
        continue;
      }

      await fontFile.create(recursive: true);
      await fontFile.writeAsBytes(fontResponse.bodyBytes, flush: true);
      stdout.writeln('[OK]   ${fontFile.path} family=${font.name} (${fontResponse.bodyBytes.length} bytes)');
      successCount++;
    } catch (e) {
      stdout.writeln('[FAIL] ${fontFile.path} family=${font.name} - $e');
      failCount++;
    }
  }

  client.close();
  stdout.writeln('\nDownload complete: $successCount success, $failCount failed.');
}
