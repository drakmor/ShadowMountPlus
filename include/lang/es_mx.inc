static const char *const g_es_mx[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Ir a configuración de depuración",
    /* English: ShadowMount+ v%s exFAT/UFS/PFS */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s exFAT/UFS/PFS",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] =
        "ShadowMount+: recursive_scan=1 está obsoleto, se usará scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: profundidad de escaneo %u activada.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] =
        "Falló el overlay de backport: %s\n%s\n0x%08X",
    /* English: Game backported: %s */
    [SM_L10N_GAME_BACKPORTED] = "Backport aplicado al juego: %s",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] =
        "Montaje de imagen rechazado:\n%s\nEl tamaño de clúster es demasiado pequeño (%llu < %u).\n"
        "Se guardó la anulación image_sector: %u.\nIntenta montar de nuevo.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] =
        "Montaje de imagen rechazado:\n%s\nEl tamaño de clúster es demasiado pequeño (%llu < %u).\n"
        "Usa un clúster más grande en la imagen o un sector size menor en la configuración.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] =
        "Falló el registro: %s (%s)\nAppInstallTitleDir no disponible",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] =
        "Falló el registro: %s (%s)\ncódigo=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Instalando: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff activo otra vez",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "KStuff pausado mientras el juego está corriendo",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Instalado: %s",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Falló el montaje de imagen: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] =
        "El tamaño de clúster del sistema de archivos (%llu) es menor que el tamaño de sector del dispositivo (%u) para %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] =
        "La imagen montada no se puede leer o está dañada",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] =
        "Caché de imágenes llena (%u entradas), no se puede rastrear el mount %s",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] =
        "Caché de imágenes llena (%u entradas), revirtiendo mount",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] =
        "Falló la instalación: %s (%s)\nTiempo agotado después de 5 minutos",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Instalación por lotes en cola (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... y %d más",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] =
        "Falló la instalación por lotes para %d app(s).\ncódigo=0x%08X",
    /* English: App crashed before KStuff pause: %s. KStuff is not to blame. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] =
        "La app falló antes de la pausa de KStuff: %s. KStuff no es la causa.",
    /* English: App crashed after KStuff pause: %s. Autotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] =
        "La app falló después de la pausa de KStuff: %s. Ajuste automático omitido.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] =
        "No se puede cargar el módulo después de la pausa de KStuff",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] =
        "%s: %s. Retardo aumentado a %u s.\nInicia el juego de nuevo.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] =
        "Crash detectado después de la pausa de KStuff: el retardo de pausa de %s aumentó a %u s. Inicia el juego de nuevo.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] =
        "Crash detectado después de la pausa de KStuff: %s. No se pudo actualizar el retardo automático.",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json faltante o no válido:\n%s",
    /* English: Found %d new games. Executing... */
    [SM_L10N_FOUND_NEW_GAMES] = "Se encontraron %d juegos nuevos. Ejecutando...",
    /* English: Library Synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Biblioteca sincronizada.\nSe encontraron %d juegos.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: configuración recargada.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] =
        "Duplicado %s ignorado:\n%s\nexistente: %s",
};
