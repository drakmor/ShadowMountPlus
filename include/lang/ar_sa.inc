static const char *const g_ar_sa[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "الانتقال إلى إعدادات التصحيح",
    /* English: ShadowMount+ v%s exFAT/UFS/PFS */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s exFAT/UFS/PFS",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "شادوماونت+: لم يعد الفحص التكراري مدعوماً، وسيتم استخدام عمق الفحص ٢.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "شادوماونت+: تم تفعيل عمق الفحص %u.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "فشل تركيب طبقة التوافق مع الإصدارات الأقدم: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "طُبّق دعم الإصدار الأقدم على اللعبة: %s",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "تم رفض تحميل الصورة:\n%s\nحجم الكتلة صغير جداً (%llu < %u).\nتم حفظ قيمة حجم القطاع الجديدة: %u.\nحاول التحميل مرة أخرى.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "تم رفض تحميل الصورة:\n%s\nحجم الكتلة صغير جداً (%llu < %u).\nاستخدم حجماً أكبر للكتلة في الصورة أو حجماً أصغر للقطاع في الإعدادات.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "فشل التسجيل: %s (%s)\nمجلد تثبيت التطبيقات غير متاح",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "تعذر التسجيل: %s (%s)\nجسر TitleDir غير متاح",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "فشل التسجيل: %s (%s)\nالرمز=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "جارٍ التثبيت: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "أصبح KStuff نشطًا من جديد",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "أُوقف KStuff مؤقتًا أثناء تشغيل اللعبة",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "تم التثبيت: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "غير معروف",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "فشل تحميل الصورة: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "حجم كتلة نظام الملفات (%llu) أصغر من حجم قطاع الجهاز (%u) لـ %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "الصورة المحمّلة غير قابلة للقراءة أو تالفة",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "ذاكرة الصور المؤقتة ممتلئة (%u إدخالًا)، ولا يمكن تتبع التركيب %s",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "ذاكرة الصور المؤقتة ممتلئة (%u إدخالًا)، جارٍ التراجع عن التركيب",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "فشل التثبيت: %s (%s)\nانتهت المهلة بعد 5 دقائق",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "تمت إضافة التثبيت الجماعي إلى قائمة الانتظار (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... و %d أخرى",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "فشل التثبيت الجماعي لـ %d تطبيق.\nالرمز=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "تعطّل التطبيق %s قبل إيقاف KStuff مؤقتًا. لم يكن KStuff هو السبب.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "تعطل %s قبل إيقاف KStuff مؤقتًا:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "تعطّل التطبيق %s بعد إيقاف KStuff مؤقتًا. تم تخطي الضبط التلقائي.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "تعطل %s بعد إيقاف KStuff مؤقتًا:\n%s\nتم تخطي الضبط التلقائي.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "تعذر تحميل الوحدة بعد إيقاف كايستاف مؤقتاً",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "تعذر تحميل الوحدة %.*s بعد إيقاف KStuff مؤقتًا",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. تمت زيادة التأخير إلى %u ثانية.\nشغّل اللعبة مرة أخرى.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "تم اكتشاف تعطل بعد إيقاف كايستاف مؤقتاً: تمت زيادة مدة الإيقاف الخاصة بـ %s إلى %u ثانية. شغّل اللعبة مرة أخرى.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "تعطل %s بعد إيقاف KStuff مؤقتًا:\n%s\nتمت زيادة التأخير إلى %u ث.\nشغّل اللعبة مجددًا.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "تم اكتشاف تعطل بعد إيقاف كايستاف مؤقتاً: %s. تعذر تحديث مدة التحسين التلقائي.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "تعطل %s بعد إيقاف KStuff مؤقتًا:\n%s\nتعذر تحديث تأخير الضبط التلقائي.",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "ملف param.json مفقود أو غير صالح:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "عُثر على %d ألعاب جديدة. جارٍ معالجتها...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "تمت مزامنة المكتبة.\nتم العثور على %d ألعاب.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: أُعيد تحميل الإعدادات.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "تم تجاهل %s المكرر:\n%s\nالموجود مسبقًا: %s",
};
