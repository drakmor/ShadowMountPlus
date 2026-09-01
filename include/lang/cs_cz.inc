static const char *const g_cs_cz[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Přejít do nastavení ladění",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor a komunita",
    /* English: ShadowMount+ startup with web interface */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor a komunita\nOvládání: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 je zastaralé, používá se scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: povolena hloubka prohledávání %u.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport overlay selhal: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Backport hry použit: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Backport hry použit: %s\nEmulátory aktualizovány",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Připojení obrazu odmítnuto:\n%s\nVelikost clusteru je příliš malá (%llu < %u).\nUloženo přepsání image_sector: %u.\nZkuste připojit znovu.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Připojení obrazu odmítnuto:\n%s\nVelikost clusteru je příliš malá (%llu < %u).\nPoužijte v obrazu větší cluster nebo v konfiguraci menší velikost sektoru.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Registrace selhala: %s (%s)\nAppInstallTitleDir není dostupné",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Registrace selhala: %s (%s)\nMost TitleDir není dostupný",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Registrace selhala: %s (%s)\nkód=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Instalace: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff je znovu aktivní",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "KStuff pozastaven, zatímco hra běží",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Nainstalováno: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Neznámé",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Připojení obrazu selhalo: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Velikost clusteru souborového systému (%llu) je menší než velikost sektoru zařízení (%u) pro %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Připojený obraz je nečitelný nebo poškozený",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Mezipaměť obrazů je plná (%u položek), připojení %s nelze sledovat",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "Mezipaměť obrazů je plná (%u položek), připojení se vrací zpět",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Instalace selhala: %s (%s)\nČasový limit po 5 minutách",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Dávková instalace ve frontě (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... a dalších %d",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Dávková instalace selhala pro %d aplikací.\nkód=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "Aplikace %s selhala před pozastavením KStuff. Příčinou není KStuff.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "Aplikace %s selhala před pozastavením KStuff:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "Aplikace %s selhala po pozastavení KStuff. Automatické ladění bylo přeskočeno.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "Aplikace %s selhala po pozastavení KStuff:\n%s\nAutomatické ladění bylo přeskočeno.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "Nelze načíst modul po pauze KStuff",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "po pozastavení KStuff nelze načíst modul %.*s",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. Zpoždění zvýšeno na %u s.\nSpusťte hru znovu.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "Po pozastavení KStuff byla zjištěna chyba: prodleva pro %s byla zvýšena na %u s. Spusťte hru znovu.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "Aplikace %s selhala po pozastavení KStuff:\n%s\nProdleva byla zvýšena na %u s.\nSpusťte hru znovu.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "Po pozastavení KStuff byla zjištěna chyba: %s. Prodlevu automatického ladění se nepodařilo aktualizovat.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "Aplikace %s selhala po pozastavení KStuff:\n%s\nProdlevu automatického ladění se nepodařilo aktualizovat.",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "Chybí nebo je neplatný param.json:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "Nalezeno %d nových her. Zpracovávám...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Knihovna synchronizována.\nNalezeno %d her.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: konfigurace byla znovu načtena.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Duplicitní %s ignorováno:\n%s\nexistující: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "Emulátor AMPR byl aktualizován",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: instalace hooků ShellCore se nezdařila",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Cílová teplota %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Externí disk %s připojen.\nVolné místo: %llu.%llu z %llu.%llu GB (blok %llu KB)\nHer na disku: %d%s",
    /* English: For maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nPro maximální výkon použijte velikost clusteru nejvýše 64 KB",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Externí disk %s odpojen",
};
