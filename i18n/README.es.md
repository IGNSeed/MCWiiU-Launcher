<p align="center">
  <img src="../src/launcher/assets/app-icon.png" alt="MCWiiU Launcher" width="112">
</p>

<h1 align="center">MCWiiU Launcher</h1>

<p align="center">Minecraft: Wii U Edition, en un solo lanzador.</p>

<p align="center">
  <strong>0.4.2 Stable — Publicado</strong><br>
  Windows 10 / 11 x64 · Cemu-based · Unofficial
</p>

<p align="center">
  <a href="../README.md">日本語</a> · <a href="README.en.md">English</a> · <a href="README.ko.md">한국어</a> · <a href="README.zh-CN.md">简体中文</a> · <strong>Español</strong>
</p>

<p align="center">
  <a href="../docs/INSTALLATION.md">Guía de instalación</a> · <a href="../docs/FEATURES.md">Guía de funciones</a>
</p>

---

## Descripción

MCWiiU Launcher es un lanzador no oficial basado en Cemu para jugar a Minecraft: Wii U Edition **v560** en un PC con Windows. Reúne la instalación del juego, la gestión de cuentas, mundos y paquetes, el inicio y las actualizaciones en una sola interfaz.

## Estado del lanzamiento

Se añadió la inicialización automática de bases de datos Mii para JPN v560. Un solo `PLAY` genera y verifica las bases ausentes, detiene normalmente la sesión de inicialización e inicia Minecraft. Las copias MLC de trabajo completadas se limpian tras verificar su propiedad e identidad de archivo. Se conservan las bases existentes, las cuentas y las partidas; las bases dañadas no se sobrescriben. Se requieren la identidad RPX verificada y recursos FFL propios. La inicialización automática de Mii para USA / EUR no está disponible. [ZIP](https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v0.4.2/MCWiiU-Launcher-0.4.2-win-x64.zip)

**[0.4.2 Stable](https://github.com/IGNSeed/MCWiiU-Launcher/releases/tag/v0.4.2) ya está disponible.** Foundation **1.0.2**, Updater Protocol **2**. [Descargar Setup](https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/v0.4.2/MCWiiU-Launcher-0.4.2-Setup-x64.exe).

Commit fuente de los binarios publicados: [c758cb39180ff28311638315ffa90b80994b0bca](https://github.com/IGNSeed/MCWiiU-Launcher/commit/c758cb39180ff28311638315ffa90b80994b0bca).

La clave pública Ed25519 de producción está integrada; se han verificado la integridad del manifiesto firmado, los archivos y las descargas públicas. Siguen pendientes los permisos de redistribución de imágenes, las comprobaciones abiertas de licencias y código fuente correspondiente, las vulnerabilidades restantes de las dependencias y la aceptación completa en instalaciones limpias de Windows 10 / 11.

## Funciones

- **Minecraft Launch / Game Data** — Inicia el juego con `PLAY` en la versión Native y juega en la misma ventana. Instala y gestiona BASE, Update v560 de la misma región y DLC opcional.
- **Account / Friends** — Gestiona cuentas y selecciona servicios en Settings; consulta la lista de Friends y las solicitudes de amistad. La disponibilidad depende de las condiciones del servicio y de la conexión.
- **World Manager** — Importa mundos, cambia nombres e iconos, mueve mundos entre Accounts y utiliza Trash y la restauración.
- **Texture Pack / Graphic Pack Manager** — Valida, selecciona y configura paquetes locales. Los cambios se aplican al iniciar de nuevo el juego.
- **Session History / Discord Rich Presence** — Consulta el tiempo de juego diario, semanal, mensual y anual y el historial por fecha; comparte la página actual y el estado de juego. Presence también depende de los ajustes de uso compartido de Discord.
- **Launcher Settings / Secure Updates** — Configura entrada, gráficos, audio y Overlay, y aplica actualizaciones firmadas del Launcher tras aprobarlas.

Durante el juego, `F10` abre la confirmación para detenerlo, `ESC` la cancela y `F11` cambia el modo de visualización. Consulta [FEATURES](../docs/FEATURES.md) para conocer los controles y las limitaciones. Que una función esté implementada no garantiza su compatibilidad con todos los PC o servicios en línea.

## Requisitos

| Componente | Requisito |
|---|---|
| OS | Windows 10 / 11 x64 |
| CPU / RAM | CPU x64. Los mínimos de CPU, instrucciones y memoria aún no están establecidos |
| GPU | GPU compatible con Vulkan u OpenGL y controladores adecuados; se prefiere Vulkan |
| Runtime | Microsoft Edge WebView2 Evergreen Runtime. Setup se detiene si falta y no lo instala automáticamente |
| Almacenamiento | Espacio para la aplicación, el juego, MLC, partidas, paquetes, cachés e instalación/actualización; mínimo aún sin fijar |

El soporte estándar de Windows 10 **finalizó el 2025-10-14**. Incluirlo como OS de destino no garantiza el soporte de seguridad del sistema. Consulta el [aviso de Microsoft](https://www.microsoft.com/en-us/windows/end-of-support).

Las pruebas de aceptación se han realizado principalmente en un único equipo de desarrollo con Windows 11, una copia aislada de un juego propio JPN v560 y una selección limitada de GPU. La aceptación completa en Windows 10, equipos limpios, todas las regiones, dispositivos de entrada/audio y sesiones prolongadas sigue pendiente. La versión de desarrollo Standalone, sin Core, no puede ejecutar el juego.

## Instalación

1. Lee [INSTALLATION](../docs/INSTALLATION.md) para conocer los requisitos y las comprobaciones de seguridad.
2. Descarga Setup de las [GitHub Releases oficiales](https://github.com/IGNSeed/MCWiiU-Launcher/releases/tag/v0.4.2) y elige un directorio de instalación. Setup **no utiliza firma Authenticode**. Sigue la guía para verificar de forma independiente el origen y la integridad del archivo.
3. Ejecuta `MCWiiU-Launcher.exe` (Bootstrapper), instala tus propios datos del juego y selecciona `PLAY` en Home. La aplicación se llama `MCWiiU Launcher.exe` y el actualizador, `MCWiiU-Updater.exe`.

La migración automática desde versiones de desarrollo anteriores no tiene soporte oficial. La guía también explica la desinstalación y la conservación de datos.

## Datos del juego

**No se incluyen el juego, Update, DLC, partidas guardadas, archivos de sistema de Wii U ni claves del juego.** Debes aportar datos propios obtenidos legalmente.

Usa Update **v560** de la misma región que BASE. El DLC es opcional. [GAME_DATA](../docs/GAME_DATA.md) explica la instalación, los cambios de región, la eliminación exclusiva de los datos del juego y la conservación de partidas. Haz una copia de seguridad de las partidas importantes antes de realizar cambios.

## Actualizaciones

Las actualizaciones normales del Launcher aplican un ZIP firmado tras la aprobación del usuario. Se verifican Ed25519, SHA-256, la vinculación con el GitHub Asset y el inventario de archivos. Si falla la comprobación Readiness de 30 segundos, Rollback vuelve a una versión anterior verificable. **La clave pública de producción está integrada. Se verificaron las firmas, la compatibilidad de preferencias y los assets públicos de 0.4.1 → 0.4.2; no se ha ejecutado una actualización automática completa.**

Foundation se actualiza **manualmente mediante Setup**, en un proceso separado de las actualizaciones normales del Launcher. Consulta [UPDATING](../docs/UPDATING.md) para conocer los detalles y la recuperación.

## Documentación

Las guías detalladas que se enlazan a continuación están actualmente en japonés.

- [Instalación y desinstalación](../docs/INSTALLATION.md)
- [Datos propios y partidas guardadas](../docs/GAME_DATA.md)
- [Controles y ajustes](../docs/FEATURES.md)
- [Actualizaciones y recuperación](../docs/UPDATING.md)
- [Solución de problemas](../docs/TROUBLESHOOTING.md)
- [Datos almacenados y conexiones](../docs/PRIVACY.md)
- [Licencias y código fuente correspondiente](../docs/THIRD_PARTY_NOTICES.md)
- [Verificación de distribución e informes de seguridad](../SECURITY.md)
- [Historial de cambios](../CHANGELOG.md)
- Desarrollo, compilación y contribuciones: [BUILD](../BUILD.md) · [CONTRIBUTING](../CONTRIBUTING.md)

Está previsto recibir informes de errores mediante GitHub Issues, pero **actualmente está desactivado y el canal de informes aún no está preparado**. No incluyas claves del juego, tokens, información de cuentas ni partidas guardadas en informes públicos.

## Créditos

Gracias a [Cemu](https://github.com/cemu-project/Cemu) y sus colaboradores, y a quienes desarrollan saucer, WebView2, Discord Social SDK y las demás dependencias. Consulta [THIRD_PARTY_NOTICES](../docs/THIRD_PARTY_NOTICES.md) para ver los avisos originales y la información sobre el código fuente correspondiente.

## Licencia

Se conservan la licencia **MPL-2.0** y los avisos de copyright del código derivado de Cemu. El texto original está en [LICENSE.txt](../LICENSE.txt). Las dependencias de terceros tienen sus propias condiciones. La licencia del código fuente no autoriza la redistribución de recursos del juego ni de imágenes de terceros.

## Aviso legal

MCWiiU Launcher no es un producto oficial de Cemu, Mojang, Microsoft ni Nintendo, ni está proporcionado o respaldado por ellos. Minecraft y los nombres y recursos relacionados pertenecen a sus respectivos titulares.
