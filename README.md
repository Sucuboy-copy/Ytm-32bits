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

- Código separado por responsabilidad: `json.cpp` interpreta JSON, renderers de videos y formatos de `playerResponse`; `innertube.cpp` realiza las peticiones y el fallback de clientes; `session.cpp` importa y protege cookies; `main.cpp` contiene la UI Win32 y Media Engine.
- Búsqueda asíncrona en `music.youtube.com/youtubei/v1/search` con `WEB_REMIX`; reconoce los renderers de música y los renderers de video de YouTube. Reutiliza el mismo `videoId` al consultar reproducción con los clientes de video.
- Selección explícita de resultado y botones separados para reproducir y pausar/reanudar; doble clic también reproduce. El registro muestra actividad y errores con hora.
- Al reproducir, prueba ANDROID_VR, ANDROID con perfil iOS anónimo, VISIONOS y TVHTML5; usa `visitorData` de la búsqueda o lo obtiene del bootstrap de YouTube. Si un cliente devuelve formatos sin URL directa (SABR), continúa con el siguiente. Prioriza URLs directas itag 251 y 140 y usa Media Engine de Windows 8+.
- Inicio de sesión mediante navegador externo e importación de `cookies.txt` en formato Netscape. Las cookies se filtran por dominio YouTube, se protegen con DPAPI en el perfil actual de Windows y se genera SAPISIDHASH por petición; nunca se registra el secreto. La sesión puede caducar y requerir otra exportación.
- La reproducción directa puede requerir una sesión de YouTube o GVS PO Token según la IP y el perfil. Windows 8.1 no trae decoder Opus en todas las instalaciones; si 251 falla, el reproductor prueba 140 y después los formatos del cliente fallback.
- Ventana ligera, lista de resultados, entrada rápida con Enter y sin dependencias de terceros.
- InnerTube cambia sin garantía de compatibilidad. Aún faltan cola, caché de URL y controles de reproducción avanzados.

## Referencia de interfaz

ArchiveTune queda anotado como inspiración para una pasada visual: secciones de inicio/Quick Picks, mini reproductor con portada y controles compactos, y colores derivados del arte. Se implementará con Win32 nativo; no se portará Jetpack Compose ni se copiará código o assets. ArchiveTune usa GPL-3.0.

FourthTube queda como referencia técnica para perfiles InnerTube, el endpoint móvil, `visitorData`, los renderers de video y el contexto Android VR. Su parser está acoplado a RapidJSON y a la red de 3DS; este cliente usa la misma estructura de respuesta de video con su parser Win32. No se copió su código fuente. FourthTube está bajo GPL-3.0.

El programa es un proyecto no oficial, sin afiliación con Google o YouTube. InnerTube es una interfaz interna y puede cambiar o dejar de responder.
