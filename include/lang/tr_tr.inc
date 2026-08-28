static const char *const g_tr_tr[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Hata ayıklama ayarlarına git",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor ve Topluluk",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: recursive_scan=1 kullanımdan kaldırıldı; scan_depth=2 kullanılacak.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: tarama derinliği %u etkin.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Backport overlay başarısız: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Oyuna backport uygulandı: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Oyuna backport uygulandı: %s\nEmülatörler güncellendi",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "İmaj bağlama işlemi reddedildi:\n%s\nKüme boyutu çok küçük (%llu < %u).\nimage_sector değeri kaydedildi: %u.\nBağlamayı yeniden deneyin.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "İmaj bağlama işlemi reddedildi:\n%s\nKüme boyutu çok küçük (%llu < %u).\nİmajda daha büyük bir küme veya yapılandırmada daha küçük bir sektör boyutu kullanın.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Kayıt başarısız: %s (%s)\nAppInstallTitleDir kullanılamıyor",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Kayıt başarısız: %s (%s)\nTitleDir köprüsü kullanılamıyor",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Kayıt başarısız: %s (%s)\nkod=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Yükleniyor: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "KStuff tekrar etkin",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "Oyun çalışırken KStuff duraklatıldı",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Yüklendi: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Bilinmiyor",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "İmaj bağlama işlemi başarısız: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Dosya sistemi küme boyutu (%llu), aygıt sektör boyutundan (%u) küçük: %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Bağlanan imaj okunamıyor veya hasarlı",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "İmaj önbelleği dolu (%u kayıt), %s bağlama işlemi izlenemiyor",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "İmaj önbelleği dolu (%u kayıt), bağlama işlemi geri alınıyor",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Yükleme başarısız: %s (%s)\n5 dakika sonra zaman aşımı",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Toplu yükleme sıraya alındı (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... ve %d tane daha",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "%d uygulama için toplu yükleme başarısız.\nkod=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "%s, KStuff duraklatılmadan önce çöktü. Sorunun nedeni KStuff değil.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "%s, KStuff duraklatılmadan önce çöktü:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "%s, KStuff duraklatıldıktan sonra çöktü. Otomatik ayarlama atlandı.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "%s, KStuff duraklatıldıktan sonra çöktü:\n%s\nOtomatik ayarlama atlandı.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "KStuff duraklamasından sonra modül yüklenemiyor",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "KStuff duraklatıldıktan sonra %.*s modülü yüklenemiyor",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. Gecikme %u s değerine çıkarıldı.\nOyunu tekrar başlatın.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "KStuff duraklatıldıktan sonra çökme algılandı: %s için gecikme %u sn. olarak artırıldı. Oyunu yeniden başlatın.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "%s, KStuff duraklatıldıktan sonra çöktü:\n%s\nGecikme %u sn. olarak artırıldı.\nOyunu yeniden başlatın.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "KStuff duraklatıldıktan sonra çökme algılandı: %s. Otomatik ayarlama gecikmesi güncellenemedi.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "%s, KStuff duraklatıldıktan sonra çöktü:\n%s\nOtomatik ayarlama gecikmesi güncellenemedi.",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "param.json eksik veya geçersiz:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "%d yeni oyun bulundu. İşleniyor...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Kütüphane senkronize edildi.\n%d oyun bulundu.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: yapılandırma yeniden yüklendi.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Yinelenen %s yok sayıldı:\n%s\nmevcut: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "AMPR emülatörü güncellendi",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: ShellCore hook'ları yüklenemedi",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Hedef sıcaklık %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Harici sürücü %s bağlandı.\nBoş: %llu.%llu / %llu.%llu GB (blok boyutu %llu KB)\nSürücüdeki oyunlar: %d%s",
    /* English: For maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nMaksimum performans için en fazla 64 KB küme boyutu kullanın",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Harici sürücü %s çıkarıldı",
};
