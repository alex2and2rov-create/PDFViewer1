# PDF Viewer — FFGL-плагин для Resolume Arena / Avenue

Source-плагин (генератор), который выводит страницы PDF как обычный клип: листание с MIDI/OSC, плавный переход между страницами, масштаб и панорамирование, поворот, прозрачная «бумага», автоперезагрузка файла при изменении.

PDF рендерится движком **PDFium** (тот же, что в Chrome) в фоновом потоке — открытие тяжёлого файла или перерендер под 4K не подвешивает вывод. Соседние страницы (+1, −1, +2) рендерятся заранее, поэтому Next/Prev на шоу срабатывают мгновенно.

## Установка

**Windows:** скопировать оба файла в `Documents\Resolume Arena\Extra Effects\`:

```
PDFViewer.dll
PDFViewer_pdfium.dll   ← обязательно рядом, это сам движок PDF
```

**macOS:** скопировать `PDFViewer.bundle` в `~/Documents/Resolume Arena/Extra Effects/`. Если macOS блокирует непод­писанный бандл:

```
xattr -dr com.apple.quarantine ~/Documents/Resolume\ Arena/Extra\ Effects/PDFViewer.bundle
```

Перезапустить Arena → вкладка **Sources** → **PDF Viewer**. Перетащить в клип, в параметре **PDF File** выбрать файл.

## Параметры

| Группа | Параметр | Что делает |
|---|---|---|
| File | **PDF File** | Файл .pdf (поддерживаются кириллица/Unicode в пути) |
| | Password | Пароль для защищённых PDF |
| | Reload | Перечитать файл с диска |
| | Auto Reload | Следить за файлом и перезагружать при сохранении (раз в секунду) — удобно, когда клиент присылает правки прямо перед шоу |
| — | **Page** | Выпадающий список страниц «3 / 24». Число страниц видно в названии параметра |
| | Prev / Next / First / Last Page | Кнопки-события — удобно мапить на MIDI-контроллер или OSC |
| | Transition | Длительность кроссфейда между страницами, 0–3 с (0 = резкая смена) |
| View | Fit | Fit (вписать с полями) / Fill (заполнить с обрезкой) / Fit Width / Fit Height / Stretch |
| | Zoom | 0.1×–10× |
| | Pan X / Pan Y | 0 % = левый/верхний край страницы у края экрана, 50 % = центр, 100 % = правый/нижний. При зуме работает как прокрутка (Fit Width + Pan Y = «телесуфлёр») |
| | Rotate | 0 / 90° CW / 180 / 90° CCW — для вертикальных экранов |
| | Quality | Разрешение растра относительно экранного: 0.5× / 1× / 1.5× / 2× |
| Look | Page Paper | White или Transparent (для PDF с вектором/логотипами поверх видео) |
| | Invert | Инверсия цветов страницы |
| | BG Red/Green/Blue/Alpha | Цвет полей вокруг страницы (Alpha 0 = прозрачные поля) |

Растр всегда делается под фактический размер страницы на выходе (с учётом Zoom), поэтому текст остаётся резким и на 4K, и при увеличении. Предел — 8192 px по большей стороне.

## Сборка из исходников

Нужны CMake 3.20+ и Visual Studio 2019+ (Windows) или Xcode (macOS). FFGL SDK, GLEW и PDFium скачиваются автоматически.

```
# Windows
cmake -B build -A x64
cmake --build build --config Release

# macOS (universal: Apple Silicon + Intel)
cmake -B build -G Xcode
cmake --build build --config Release
```

Готовые файлы — в `build/dist/`. Если залить проект в GitHub, workflow в `.github/workflows/build.yml` сам соберёт Windows- и macOS-версии (вкладка Actions → Artifacts).

## Устройство

- `src/FFGLPdfViewer.*` — сам плагин: параметры, загрузка текстур, шейдер (fit/zoom/pan/rotate, кроссфейд, фон).
- `src/PdfRenderer.*` — фоновый поток: загрузка PDF, рендер страниц, LRU-кэш (до 768 МБ на экземпляр), предзагрузка соседних страниц, авто-reload.
- `src/PdfiumLoader.*` — PDFium грузится по полному пути под уникальным именем `PDFViewer_pdfium.dll`, чтобы не конфликтовать с чужими копиями pdfium.dll в процессе Resolume.
- `tests/render_test.cpp` — консольный тест рендера без Resolume (`-DPDFVIEWER_BUILD_TEST=ON`).

## Ограничения

- Интерактивные формы и JavaScript в PDF не исполняются; аннотации и видимые поля форм отображаются.
- Анимации/переходы PowerPoint, экспортированного в PDF, не сохраняются — это ограничение формата PDF.

## Лицензии

PDFium — BSD-3-Clause / Apache 2.0 (лицензии сторонних компонентов в папке `licenses/`). FFGL SDK — BSD (Resolume).
