# Ytm32

Cliente nativo mínimo para YouTube Music en Windows 8.1 x86. La base contiene una interfaz Win32/GDI oscura y búsqueda InnerTube por HTTPS usando WinHTTP. No incluye Chromium, Electron ni un runtime de JavaScript.

## Compilar

Requiere Visual Studio 2019 o Build Tools con el workload de C++ para escritorio, Windows 8.1 SDK y CMake 3.15 o posterior. Desde un Developer Command Prompt x86:

```bat
cmake -S . -B build -A Win32
cmake --build build --config Release
```

El binario usa el runtime estático del CRT y enlaza WinHTTP y Common Controls del sistema.

Para compilar un ejecutable x86 desde Linux con MinGW-w64:

```sh
cmake -S . -B /tmp/ytm32-build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw32.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/ytm32-build --parallel
```

## Estado

- Código separado por responsabilidad: `json.cpp` interpreta JSON, renderers de videos y formatos de `playerResponse`; `innertube.cpp` realiza las peticiones y el fallback de clientes; `oauth.cpp` gestiona OAuth de dispositivo; `opus_decoder.cpp` desempaqueta WebM y decodifica Opus; `main.cpp` contiene la UI Win32 y Media Engine.
- Búsqueda asíncrona en `music.youtube.com/youtubei/v1/search` con `WEB_REMIX`; reconoce los renderers de música y los renderers de video de YouTube. Conserva `visitorData` entre búsquedas y reproducción, tanto en el contexto JSON como en `X-Goog-Visitor-Id` cuando está disponible. Las peticiones envían versión de formato, idioma, origen y referer junto con el perfil del cliente.
- Interfaz Win32 inspirada en ArchiveTune: navegación lateral, inicio con búsquedas rápidas, vistas Explorar y Mi música, y mini reproductor persistente.
- Favoritos locales persistentes, cola desde resultados o favoritos, controles anterior/siguiente y avance automático al terminar una canción. También puede abrir archivos locales MP3, WAV, WMA, M4A y AAC mediante Media Engine (los formatos disponibles dependen de los decodificadores de Windows).
- Las filas y el mini reproductor muestran portadas descargadas en segundo plano con WinHTTP y decodificadas con Windows Imaging Component. La barra del mini reproductor muestra el tiempo y permite buscar en la pista con un clic.
- Selección explícita de resultado y doble clic para reproducir; el registro muestra actividad y errores con hora.
- Al reproducir, busca con ANDROID_VR autenticado mediante Bearer OAuth y continúa con ANDROID, VISIONOS y TVHTML5 cuando hacen falta. Reutiliza el `videoId` de la búsqueda, y si un cliente devuelve formatos sin URL directa (SABR), continúa con el siguiente. Prioriza itag 251 y luego 140.
- El login usa OAuth Device Flow de Android VR, como FourthTube: abre la verificación en el navegador, introduce el código y el programa conserva el refresh token cifrado con DPAPI. Las cookies Netscape antiguas permanecen como credencial opcional para las peticiones WEB_REMIX; los secretos no se escriben en el registro.
- itag 251 se descarga en segundo plano, se desempaqueta desde WebM y se decodifica con libopus 1.6.1 incluida estáticamente; el PCM resultante se mantiene como WAV en memoria (sin archivos temporales) y Media Engine lo lee mediante un `IMFByteStream` sobre un `IStream` de solo lectura. El buffer se reserva de una vez usando la duración del WebM y tiene un límite de 256 MiB de PCM (~23 min en estéreo); si se excede o falla el stream Opus, se intenta itag 140.
- Ventana nativa ligera, lista de resultados, entrada rápida con Enter y libopus estática para el formato Opus.
- InnerTube cambia sin garantía de compatibilidad. Aún faltan cola, caché de URL y controles de reproducción avanzados.

## Referencia de interfaz

ArchiveTune es la musa visual y funcional para inicio/Quick Picks, navegación por secciones, biblioteca local y mini reproductor. La interfaz se implementa con Win32 nativo; no se portan Jetpack Compose ni sus assets. ArchiveTune usa GPL-3.0.

FourthTube queda como referencia técnica para perfiles InnerTube, el endpoint móvil, `visitorData`, los renderers de video y el contexto Android VR. Su parser está acoplado a RapidJSON y a la red de 3DS; este cliente usa la misma estructura de respuesta de video con su parser Win32. No se copió su código fuente. FourthTube está bajo GPL-3.0.

[`rukamori/core`](https://github.com/rukamori/core/tree/fefeefa4a8e74d80b262ad944f5adca6b1a897f7) sirve de referencia para separar transporte, contexto de cliente y estado de autenticación. Su implementación usa Kotlin/JVM y Ktor, así que Ytm32 conserva WinHTTP y adapta esos conceptos al cliente nativo; no se incorporó código fuente de ese proyecto. `rukamori/core` está bajo GPL-3.0.

El programa es un proyecto no oficial, sin afiliación con Google o YouTube. InnerTube es una interfaz interna y puede cambiar o dejar de responder.
