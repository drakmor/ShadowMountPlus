static const char *const g_el_gr[SM_L10N_COUNT] = {
    /* English: Go to Debug Settings */
    [SM_L10N_ACTION_DEBUG_SETTINGS] = "Μετάβαση στις ρυθμίσεις αποσφαλμάτωσης",
    /* English: ShadowMount+ v%s\n© Drakmor and Community */
    [SM_L10N_STARTUP] = "ShadowMount+ v%s\n© Drakmor και η κοινότητα",
    /* English: ShadowMount+ startup with web interface */
    [SM_L10N_STARTUP_WEB] =
        "ShadowMount+ v%s\n© Drakmor και η κοινότητα\nΈλεγχος: http://%s:%u",
    /* English: ShadowMount+: recursive_scan=1 is deprecated, using scan_depth=2. */
    [SM_L10N_RECURSIVE_SCAN_DEPRECATED] = "ShadowMount+: το recursive_scan=1 είναι παρωχημένο, χρησιμοποιείται scan_depth=2.",
    /* English: ShadowMount+: scan depth %u enabled. */
    [SM_L10N_SCAN_DEPTH_ENABLED] = "ShadowMount+: ενεργό βάθος σάρωσης %u.",
    /* English: Backport overlay failed: %s\n%s\n0x%08X */
    [SM_L10N_BACKPORT_OVERLAY_FAILED] = "Αποτυχία backport overlay: %s\n%s\n0x%08X",
    /* English: Backport applied to game: %s */
    [SM_L10N_GAME_BACKPORTED] = "Εφαρμόστηκε backport στο παιχνίδι: %s",
    /* English: Backport applied to game: %s\nEmulators updated */
    [SM_L10N_GAME_BACKPORTED_EMULATORS_UPDATED] = "Εφαρμόστηκε backport στο παιχνίδι: %s\nΟι εξομοιωτές ενημερώθηκαν",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nSaved image_sector override: %u.\nTry mounting again. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_AUTOTUNED] = "Η προσάρτηση του ειδώλου απορρίφθηκε:\n%s\nΤο μέγεθος συστοιχίας είναι πολύ μικρό (%llu < %u).\nΑποθηκεύτηκε η τιμή image_sector: %u.\nΔοκιμάστε ξανά την προσάρτηση.",
    /* English: Image mount rejected:\n%s\nCluster size is too small (%llu < %u).\nUse a larger cluster size in the image or lower sector size in config. */
    [SM_L10N_IMAGE_REJECTED_CLUSTER_CONFIG] = "Η προσάρτηση του ειδώλου απορρίφθηκε:\n%s\nΤο μέγεθος συστοιχίας είναι πολύ μικρό (%llu < %u).\nΧρησιμοποιήστε μεγαλύτερη συστοιχία στο είδωλο ή μικρότερο μέγεθος τομέα στις ρυθμίσεις.",
    /* English: Register failed: %s (%s)\nAppInstallTitleDir unavailable */
    [SM_L10N_REGISTER_FAILED_UNAVAILABLE] = "Αποτυχία εγγραφής: %s (%s)\nΤο AppInstallTitleDir δεν είναι διαθέσιμο",
    /* English: Register failed: %s (%s)\nTitleDir bridge unavailable */
    [SM_L10N_REGISTER_BRIDGE_UNAVAILABLE] = "Η εγγραφή απέτυχε: %s (%s)\nΗ γέφυρα TitleDir δεν είναι διαθέσιμη",
    /* English: Register failed: %s (%s)\ncode=0x%08X */
    [SM_L10N_REGISTER_FAILED_CODE] = "Αποτυχία εγγραφής: %s (%s)\nκωδικός=0x%08X",
    /* English: Installing: %s (%s)... */
    [SM_L10N_INSTALLING] = "Εγκατάσταση: %s (%s)...",
    /* English: KStuff active again */
    [SM_L10N_KSTUFF_ACTIVE] = "Το KStuff είναι ξανά ενεργό",
    /* English: KStuff paused while the game is running */
    [SM_L10N_KSTUFF_PAUSED] = "Το KStuff παύθηκε όσο τρέχει το παιχνίδι",
    /* English: Installed: %s */
    [SM_L10N_INSTALLED] = "Εγκαταστάθηκε: %s",
    /* English: Unknown */
    [SM_L10N_UNKNOWN] = "Άγνωστο",
    /* English: Image mount failed: 0x%08X (%s)\n%s */
    [SM_L10N_IMAGE_MOUNT_FAILED] = "Η προσάρτηση του ειδώλου απέτυχε: 0x%08X (%s)\n%s",
    /* English: Filesystem cluster size (%llu) is smaller than device sector size (%u) for %s */
    [SM_L10N_FS_CLUSTER_SMALL] = "Το μέγεθος cluster του συστήματος αρχείων (%llu) είναι μικρότερο από το μέγεθος sector της συσκευής (%u) για %s",
    /* English: Mounted image is unreadable or damaged */
    [SM_L10N_IMAGE_UNREADABLE_DAMAGED] = "Το mounted image δεν διαβάζεται ή είναι κατεστραμμένο",
    /* English: Image cache full (%u entries), cannot track mount %s */
    [SM_L10N_IMAGE_CACHE_FULL_TRACK] = "Η προσωρινή μνήμη ειδώλων είναι πλήρης (%u εγγραφές), δεν είναι δυνατή η παρακολούθηση της προσάρτησης %s",
    /* English: Image cache full (%u entries), rolling back mount */
    [SM_L10N_IMAGE_CACHE_FULL_ROLLBACK] = "Η προσωρινή μνήμη ειδώλων είναι πλήρης (%u εγγραφές), γίνεται αναίρεση της προσάρτησης",
    /* English: Install failed: %s (%s)\nTimed out after 5 minutes */
    [SM_L10N_INSTALL_FAILED_TIMEOUT] = "Αποτυχία εγκατάστασης: %s (%s)\nTimeout μετά από 5 λεπτά",
    /* English: Batch install queued (%d): */
    [SM_L10N_BATCH_INSTALL_QUEUED] = "Μαζική εγκατάσταση στην ουρά (%d):",
    /* English: \n... and %d more */
    [SM_L10N_BATCH_INSTALL_AND_MORE] = "\n... και %d ακόμα",
    /* English: Batch install failed for %d app(s).\ncode=0x%08X */
    [SM_L10N_BATCH_INSTALL_FAILED] = "Η μαζική εγκατάσταση απέτυχε για %d app(s).\nκωδικός=0x%08X",
    /* English: %s crashed before KStuff was paused. KStuff is not the cause. */
    [SM_L10N_CRASH_BEFORE_KSTUFF] = "Η εφαρμογή %s κατέρρευσε πριν από την παύση του KStuff. Δεν ευθύνεται το KStuff.",
    /* English: %s crashed before KStuff pause:\n%s */
    [SM_L10N_CRASH_BEFORE_KSTUFF_FATAL] = "Το %s κατέρρευσε πριν από την παύση του KStuff:\n%s",
    /* English: %s crashed after KStuff was paused. Automatic tuning skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_SKIPPED] = "Η εφαρμογή %s κατέρρευσε μετά την παύση του KStuff. Η αυτόματη ρύθμιση παραλείφθηκε.",
    /* English: %s crashed after KStuff pause:\n%s\nAutotune skipped. */
    [SM_L10N_CRASH_AFTER_KSTUFF_FATAL_SKIPPED] = "Το %s κατέρρευσε μετά την παύση του KStuff:\n%s\nΗ αυτόματη ρύθμιση παραλείφθηκε.",
    /* English: Can't load module after KStuff pause */
    [SM_L10N_RTLD_AFTER_KSTUFF_FALLBACK] = "Δεν μπορεί να φορτωθεί module μετά την παύση KStuff",
    /* English: can't load module %.*s after KStuff pause */
    [SM_L10N_RTLD_MODULE_AFTER_KSTUFF] = "δεν είναι δυνατή η φόρτωση της μονάδας %.*s μετά την παύση του KStuff",
    /* English: %s: %s. Delay increased to %us.\nLaunch the game again. */
    [SM_L10N_DELAY_INCREASED_RELAUNCH] = "%s: %s. Η καθυστέρηση αυξήθηκε σε %u s.\nΕκκινήστε ξανά το παιχνίδι.",
    /* English: Crash detected after KStuff pause: pause delay for %s increased to %us. Launch the game again. */
    [SM_L10N_CRASH_DELAY_INCREASED] = "Εντοπίστηκε κατάρρευση μετά την παύση του KStuff: η καθυστέρηση για το %s αυξήθηκε σε %u δ. Εκκινήστε ξανά το παιχνίδι.",
    /* English: %s crashed after KStuff pause:\n%s\nDelay increased to %us.\nLaunch the game again. */
    [SM_L10N_CRASH_FATAL_DELAY_INCREASED] = "Το %s κατέρρευσε μετά την παύση του KStuff:\n%s\nΗ καθυστέρηση αυξήθηκε σε %u δ.\nΕκκινήστε ξανά το παιχνίδι.",
    /* English: Crash detected after KStuff pause: %s. Failed to update autotune delay. */
    [SM_L10N_CRASH_DELAY_UPDATE_FAILED] = "Εντοπίστηκε κατάρρευση μετά την παύση του KStuff: %s. Αποτυχία ενημέρωσης της καθυστέρησης αυτόματης ρύθμισης.",
    /* English: %s crashed after KStuff pause:\n%s\nFailed to update autotune delay. */
    [SM_L10N_CRASH_FATAL_DELAY_UPDATE_FAILED] = "Το %s κατέρρευσε μετά την παύση του KStuff:\n%s\nΑποτυχία ενημέρωσης της καθυστέρησης αυτόματης ρύθμισης.",
    /* English: Missing/invalid param.json:\n%s */
    [SM_L10N_MISSING_PARAM] = "Λείπει ή είναι άκυρο το param.json:\n%s",
    /* English: Found %d new games. Processing... */
    [SM_L10N_FOUND_NEW_GAMES] = "Βρέθηκαν %d νέα παιχνίδια. Γίνεται επεξεργασία...",
    /* English: Library synchronized.\nFound %d games. */
    [SM_L10N_LIBRARY_SYNCED] = "Η βιβλιοθήκη συγχρονίστηκε.\nΒρέθηκαν %d παιχνίδια.",
    /* English: ShadowMount+: config reloaded. */
    [SM_L10N_CONFIG_RELOADED] = "ShadowMount+: οι ρυθμίσεις επαναφορτώθηκαν.",
    /* English: Duplicate %s ignored:\n%s\nexisting: %s */
    [SM_L10N_DUPLICATE_IGNORED] = "Το διπλότυπο %s αγνοήθηκε:\n%s\nυπάρχον: %s",
    /* English: AMPR emulator updated */
    [SM_L10N_AMPR_EMULATOR_UPDATED] = "Ο εξομοιωτής AMPR ενημερώθηκε",
    /* English: ShadowMount+: failed to install ShellCore hooks */
    [SM_L10N_SHELLCORE_HOOKS_FAILED] = "ShadowMount+: απέτυχε η εγκατάσταση των hooks του ShellCore",
    /* English: Target temperature %u °C → %u °C */
    [SM_L10N_TEMPERATURE_TRANSITION] = "Θερμοκρασία-στόχος %u °C → %u °C",
    /* English: External drive %s connected\nFree %llu.%llu GB of %llu.%llu (Block size %llu KB)\nGames on drive: %d */
    [SM_L10N_USB_CONNECTED_SCANNING] = "Ο εξωτερικός δίσκος %s συνδέθηκε.\nΕλεύθερα: %llu.%llu από %llu.%llu GB (μέγεθος μπλοκ %llu KB)\nΠαιχνίδια στον δίσκο: %d%s",
    /* English: For maximum performance, use a cluster size of no more than 64 KB */
    [SM_L10N_USB_CLUSTER_RECOMMENDATION] = "\nΓια μέγιστη απόδοση, χρησιμοποιήστε μέγεθος συμπλέγματος έως 64 KB",
    /* English: External drive %s disconnected */
    [SM_L10N_USB_DISCONNECTED] = "Ο εξωτερικός δίσκος %s αποσυνδέθηκε",
};
