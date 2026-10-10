static const char *const g_fr_ca[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Accéder aux paramètres de débogage",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor et la communauté",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor et la communauté\nContrôle : http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+ : recursive_scan=1 est obsolète, scan_depth=2 sera utilisé.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+ : profondeur d’analyse %u activée.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Échec du montage de la surcouche de backport : %s\n%s\n0x%08X",
    /* English: Emulators applied to game: %s */
    [SM_L10N_GAME_EMULATORS_APPLIED] = "Émulateurs appliqués au jeu : %s",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Backport appliqué au jeu : %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Backport appliqué au jeu : %s\nÉmulateurs mis à jour",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Montage de l’image refusé :\n%s\nLa taille des grappes est trop petite (%llu < %u).\nValeur image_sector enregistrée : %u.\nRéessayez de monter l’image.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Montage de l’image refusé :\n%s\nLa taille des grappes est trop petite (%llu < %u).\nUtilisez des grappes plus grandes dans l’image ou une taille de secteur plus petite dans la configuration.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Échec de l'enregistrement : %s (%s)\nAppInstallTitleDir indisponible",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Échec de l’enregistrement : %s (%s)\nLe pont TitleDir n’est pas disponible",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Échec de l'enregistrement : %s (%s)\ncode=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Installation : %s (%s)...",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Installé : %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Inconnu",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Échec du montage de l'image : 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "La taille des grappes du système de fichiers (%llu) est inférieure à la taille des secteurs du périphérique (%u) pour %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "L'image montée est illisible ou endommagée",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Le cache d’images est plein (%u entrées), impossible de suivre le montage %s",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Échec de l'installation : %s (%s)\nDélai expiré après 5 minutes",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Installation par lot mise en file (%d) :",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... et %d de plus",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Échec de l'installation par lot pour %d application(s).\ncode=0x%08X",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json manquant ou invalide :\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "%d nouveaux jeux trouvés. Traitement en cours...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Bibliothèque synchronisée.\n%d jeux trouvés.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+ : configuration rechargée.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Doublon %s ignoré :\n%s\nexistant : %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "Émulateur AMPR mis à jour",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+ : échec de l’installation des hooks ShellCore",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Température cible %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Lecteur externe %s connecté.\nLibre : %llu.%llu sur %llu.%llu Go (bloc %llu Ko)\nJeux sur le lecteur : %d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nPour des performances maximales, utilisez une taille de cluster ne dépassant pas 64 Ko",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Lecteur externe %s déconnecté",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "Le jeu %s a planté.",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "Erreur dans le jeu %s :\n%s",
    /* English: External drive %s connected */
    [SM_L10N_USB_CONNECTED] = "Lecteur externe %s connecté.",
};
