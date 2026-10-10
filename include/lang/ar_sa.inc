static const char *const g_ar_sa[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "الانتقال إلى إعدادات التصحيح",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor والمجتمع",
    /* English: ShadowMount+ v%s\n© Drakmor and Community\nControl: http://%s:%u */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor والمجتمع\nالتحكم: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: الخيار recursive_scan=1 مهجور، وسيُستخدم scan_depth=2 بدلًا منه.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: تم تمكين عمق المسح %u.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "فشل تطبيق طبقة الدعم الرجعي: %s\n%s\n0x%08X",
    /* English: Emulators applied to game: %s */
    [SM_L10N_GAME_EMULATORS_APPLIED] = "تم تطبيق المحاكيات على اللعبة: %s",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "طُبّق الدعم الرجعي على اللعبة: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "طُبّق الدعم الرجعي على اللعبة: %s\nتم تحديث المحاكيات",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "تم رفض تركيب صورة القرص:\n%s\nحجم العنقود صغير جدًا (%llu < %u).\nتم حفظ تجاوز قطاع صورة القرص: %u.\nحاول التركيب مرة أخرى.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "تم رفض تركيب صورة القرص:\n%s\nحجم العنقود صغير جدًا (%llu < %u).\nاستخدم حجمًا أكبر للعنقود في صورة القرص أو حجمًا أصغر للقطاع في الإعدادات.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "فشل التسجيل: %s (%s)\nAppInstallTitleDir غير متاح",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "فشل التسجيل: %s (%s)\nجسر TitleDir غير متاح",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "فشل التسجيل: %s (%s)\nالرمز=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "جارٍ التثبيت: %s (%s)...",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "تم التثبيت: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "غير معروف",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "فشل تركيب صورة القرص: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "حجم عنقود نظام الملفات (%llu) أصغر من حجم قطاع الجهاز (%u) للجهاز %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "صورة القرص المركّبة غير قابلة للقراءة أو تالفة",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "ذاكرة صور القرص المؤقتة ممتلئة (%u إدخالًا)، لا يمكن تتبع التركيب %s",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "فشل التثبيت: %s (%s)\nانتهت المهلة بعد 5 دقائق",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "تم وضع التثبيت الجماعي في قائمة الانتظار (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... و %d إضافية",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "فشل التثبيت الجماعي لـ %d من التطبيقات.\nالرمز=0x%08X",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "ملف param.json مفقود أو غير صالح:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "عُثر على %d من الألعاب الجديدة. جارٍ معالجتها...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "تمت مزامنة المكتبة.\nعُثر على %d من الألعاب.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: تمت إعادة تحميل الإعدادات.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "تم تجاهل %s المكرر:\n%s\nالموجود مسبقًا: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "تم تحديث محاكي AMPR",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: تعذر تثبيت خطافات ShellCore",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "درجة الحرارة المستهدفة %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d%s */
    [SM_L10N_USB_CONNECTED_SCANNING] = "تم توصيل محرك الأقراص الخارجي %s\nالمساحة المتاحة %llu.%llu من %llu.%llu غيغابايت (حجم الكتلة %llu كيلوبايت)\nالألعاب على القرص: %d%s",
    /* English: \nFor maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nللحصول على أقصى أداء، استخدم حجم عنقود لا يزيد عن 64 كيلوبايت",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "تم فصل محرك الأقراص الخارجي %s",
    /* English: Game %s crashed. */
    [SM_L10N_GAME_CRASHED] = "تعطّلت اللعبة %s.",
    /* English: Error in game %s:\n%s */
    [SM_L10N_GAME_ERROR] = "خطأ في اللعبة %s:\n%s",
    /* English: External drive %s connected */
    [SM_L10N_USB_CONNECTED] = "تم توصيل محرك الأقراص الخارجي %s",
};
