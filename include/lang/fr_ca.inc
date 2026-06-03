static const char *const g_fr_ca[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Aller aux paramètres de débogage",
    /* English: ShadowMount+ v%s exFAT/UFS/PFS */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s exFAT/UFS/PFS",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] =
        "ShadowMount+ : recursive_scan=1 est désuet, scan_depth=2 sera utilisé.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+ : profondeur de scan %u activée.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] =
        "Échec de l'overlay backport : %s\n%s\n0x%08X",
    /* English: Game backported: %s */
    [SM_L10N_GAME_BACKPORTED] = "Jeu backporté : %s",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] =
        "Montage de l'image refusé :\n%s\nLa taille de cluster est trop petite (%llu < %u).\n"
        "Remplacement image_sector enregistré : %u.\nEssayez de remonter.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] =
        "Montage de l'image refusé :\n%s\nLa taille de cluster est trop petite (%llu < %u).\n"
        "Utilisez un cluster plus grand dans l'image ou une sector size plus petite dans la config.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] =
        "Échec de l'enregistrement : %s (%s)\nAppInstallTitleDir indisponible",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] =
        "Échec de l'enregistrement : %s (%s)\ncode=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Installation : %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff est de nouveau actif",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "KStuff est en pause pendant que le jeu roule",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Installé : %s",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Échec du montage de l'image : 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] =
        "La taille de cluster du système de fichiers (%llu) est inférieure à la taille de secteur du périphérique (%u) pour %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] =
        "L'image montée est illisible ou endommagée",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] =
        "Cache d'images plein (%u entrées), impossible de suivre le mount %s",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] =
        "Cache d'images plein (%u entrées), annulation du mount",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] =
        "Échec de l'installation : %s (%s)\nDélai expiré après 5 minutes",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Installation par lot en file (%d) :",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... et %d de plus",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] =
        "Échec de l'installation par lot pour %d application(s).\ncode=0x%08X",
    /* English: App crashed before KStuff pause: %s. KStuff is not to blame. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] =
        "L'application a planté avant la pause KStuff : %s. KStuff n'est pas en cause.",
    /* English: App crashed after KStuff pause: %s. Autotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] =
        "L'application a planté après la pause KStuff : %s. Réglage auto ignoré.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] =
        "Impossible de charger le module après la pause KStuff",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] =
        "%s : %s. Délai augmenté à %u s.\nRelancez le jeu.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] =
        "Crash détecté après la pause KStuff : délai de pause pour %s augmenté à %u s. Relancez le jeu.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] =
        "Crash détecté après la pause KStuff : %s. Échec de la mise à jour du délai auto.",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json manquant ou invalide :\n%s",
    /* English: Found %d new games. Executing... */
    [SM_L10N_FOUND_NEW_GAMES] = "%d nouveaux jeux trouvés. Exécution...",
    /* English: Library Synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Bibliothèque synchronisée.\n%d jeux trouvés.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+ : config rechargée.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] =
        "Doublon %s ignoré :\n%s\nexistant : %s",
};
