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
- Búsqueda asíncrona en `music.youtube.com/youtubei/v1/search` con `WEB_REMIX`; reconoce los renderers de música y los renderers de video de YouTube. Reutiliza el mismo `videoId` al consultar reproducción con los clientes de video.
- Selección explícita de resultado y botones separados para reproducir y pausar/reanudar; doble clic también reproduce. El registro muestra actividad y errores con hora.
- Al reproducir, busca con ANDROID_VR autenticado mediante Bearer OAuth y continúa con ANDROID, VISIONOS y TVHTML5 cuando hacen falta. Reutiliza el `videoId` de la búsqueda, y si un cliente devuelve formatos sin URL directa (SABR), continúa con el siguiente. Prioriza itag 251 y luego 140.
- El login usa OAuth Device Flow de Android VR, como FourthTube: abre la verificación en el navegador, introduce el código y el programa conserva el refresh token cifrado con DPAPI. Las cookies Netscape antiguas permanecen como credencial opcional para las peticiones WEB_REMIX; los secretos no se escriben en el registro.
- itag 251 se descarga en segundo plano, se desempaqueta desde WebM y se decodifica con libopus 1.6.1 incluida estáticamente; Media Engine recibe WAV PCM temporal y se elimina al cerrar la aplicación. Si el stream Opus falla, se intenta itag 140. La conversión requiere espacio temporal adicional proporcional a la duración.
- Ventana nativa ligera, lista de resultados, entrada rápida con Enter y libopus estática para el formato Opus.
- InnerTube cambia sin garantía de compatibilidad. Aún faltan cola, caché de URL y controles de reproducción avanzados.

## Referencia de interfaz

ArchiveTune queda anotado como inspiración para una pasada visual: secciones de inicio/Quick Picks, mini reproductor con portada y controles compactos, y colores derivados del arte. Se implementará con Win32 nativo; no se portará Jetpack Compose ni se copiará código o assets. ArchiveTune usa GPL-3.0.

FourthTube queda como referencia técnica para perfiles InnerTube, el endpoint móvil, `visitorData`, los renderers de video y el contexto Android VR. Su parser está acoplado a RapidJSON y a la red de 3DS; este cliente usa la misma estructura de respuesta de video con su parser Win32. No se copió su código fuente. FourthTube está bajo GPL-3.0.

El programa es un proyecto no oficial, sin afiliación con Google o YouTube. InnerTube es una interfaz interna y puede cambiar o dejar de responder.
