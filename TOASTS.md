# Toast Formats

This file collects the notification payload formats and behavior reconstructed from notification overlay bundle analysis.

The focus is practical:

- which fields the overlay expects
- which template families exist
- what `viewData` can contain
- what action types and placeholder types are supported
- what replacement/suppression logic exists
- which parts are safer for local payload-generated notifications

## Scope

The bundle contains both:

- informative toasts
- interactive/rich toasts
- indicator-style notifications
- special templates such as game preparation, trophy, friend request, share play

For local JSON payloads sent through the notification pipeline, the main entry
points and validators are visible in the bundle.


## High-Level Payload Shape

Typical notification model shape:

```json
{
  "rawData": {
    "viewTemplateType": "...",
    "useCaseId": "...",
    "channelType": "...",
    "toastOverwriteType": "No",
    "isImmediate": true,
    "priority": 100,
    "bundleName": "...",
    "viewData": { ... },
    "platformViews": { ... },
    "platformParameters": { ... }
  },
  "createdDateTime": "2026-03-09T12:00:00.000Z",
  "updatedDateTime": "2026-03-09T12:00:00.000Z",
  "expirationDateTime": "2026-03-09T13:00:00.000Z",
  "localNotificationId": "123456789",
  "notificationId": "399498519847690",
  "offConsoleToastType": "Always",
  "state": "New",
  "userId": 254,
  "isAnonymous": false,
  "userIdBackup": 0,
  "fromUser": { ... },
  "associatedTitle": { ... },
  "uploadData": { ... }
}
```

For local `"send"` events, the overlay parses the JSON and injects
`createdDateTime` before forwarding the payload internally.


## Required Fields

The bundle validates these fields on `rawData` as required:

- `viewTemplateType`
- `useCaseId`
- `channelType`
- `viewData`


If one is missing, the overlay records a client error.

## Validation and Failure Modes

The bundle shows several distinct failure paths for incoming/local payloads.

### JSON parsing

For local `"send"` events, the payload is parsed with `JSON.parse(...)`.

Behavior:

- malformed JSON raises an exception
- the exception is converted to a client error event
- the notification is dropped


### Missing required `rawData` members

If one of the required members is missing from `rawData`:

- the overlay emits a client error event naming the missing fields
- the payload may still continue through the pipeline, but it is already in an
  invalid state and later rendering/routing can fail

Practical rule:

- treat the four required `rawData` fields as hard requirements, not warnings


### Unknown `viewTemplateType`

If `viewTemplateType` cannot be resolved to a registered template:

- the overlay logs a warning
- a client error event is emitted
- the toast cannot render normally


### Unsupported generic `actionType`

In the generic action renderer:

- if `action` is missing, nothing is rendered
- if `action.actionType` is unsupported, nothing is rendered

Practical rule:

- unsupported action types fail softly by producing no visible button


### Empty converted payload

If conversion/filtering produces an empty payload:

- the overlay emits a client error
- dispatch is aborted


## Hard Constraints and Behavioral Requirements

These are the most useful bundle-backed rules if the goal is not just to mimic
sample JSON, but to produce payloads that the overlay will consistently accept
and render.

### 1. `viewTemplateType` must resolve to a known renderer

If the overlay cannot map `viewTemplateType` to a registered template, it logs
an unknown-template warning and cannot render the payload normally.

Practical rule:

- use only template names exported by the bundle
- do not invent new `viewTemplateType` values


### 2. `localNotificationId` is optional, but if present it must be non-empty

The bundle explicitly rejects empty `localNotificationId` values when local
notification cache/update paths use them.

Practical rule:

- omit `localNotificationId` entirely, or
- provide a non-empty string


### 3. `createdDateTime` should be a real ISO8601 UTC string

The overlay uses `createdDateTime` for ordering and compares it as a string
after trimming the trailing `Z`.

Practical rule:

- use canonical UTC timestamps such as `2026-03-10T12:34:56.000Z`
- keep them lexicographically sortable


### 4. Replacement depends on `userId`, `toastOverwriteType`, `bundleName`, and `useCaseId`

Replacement does not happen globally. The bundle checks:

1. same `userId`
2. exact `id` match, if present
3. overwrite policy allows replacement
4. same `bundleName`, or same `useCaseId` only when `bundleName` is absent

`platformParameters.toastOverwriteType` can override the main
`toastOverwriteType`.

The `id` check is part of the normalized internal notification model. It is the
strongest replacement key, but ordinary local JSON examples do not typically
set it directly.

Practical rule:

- if you want controlled replacement, keep `userId` stable
- prefer a stable `bundleName`
- use `toastOverwriteType` intentionally:
  - `No` for standalone items
  - `InQueue` / `Always` only when replacement semantics are desired


### 5. `previewDisabled` is interactive-only

The bundle applies `platformViews.previewDisabled` to interactive toasts.
Sample coverage in the bundle explicitly shows that informative toasts do not
use this alternate view.

Practical rule:

- only rely on `platformViews.previewDisabled` for `InteractiveToast*`
- do not expect `ToastTemplateA/B` to switch to that preview variant


### 6. VR-specific platform views are optional and mode-dependent

`platformViews.virtualReality3D` and `platformViews.virtualReality3D2` are not
generic fallback fields; they are selected only in specific VR/display modes.

Practical rule:

- include them only if you actually target VR-specific behavior
- do not rely on them for normal flat-mode notification rendering


### 7. Placeholder object types are not arbitrary

For title-oriented expansion/prefetch and placeholder processing, the bundle
explicitly recognizes:

- `User`
- `Title`
- `ConceptByTitle`
- `TitleByTitle`

For some title-preload flows, only the title-like subset is harvested:

- `Title`
- `ConceptByTitle`
- `TitleByTitle`

Practical rule:

- do not invent custom `objectType` values unless you have evidence that the
  target renderer accepts them
- for generic local toasts, stick to the known built-ins above


### 8. Generic `actions` only support a limited set of action renderers

The generic action renderer maps only recognized `actionType` values to button
components. In the bundle, the generic mapping clearly supports:

- `DeepLink`
- `ActionCardLink`

`PlayerControl` exists, but appears in specialized scenario handlers rather than
the generic action factory path.

Practical rule:

- for custom generic interactive toasts, use `DeepLink` or `ActionCardLink`
- use `PlayerControl` only when following a known specialized template path


### 9. `GameCTA` is special-purpose

`GameCTA` exists as a specialized UI primitive and is not a generic substitute
for arbitrary buttons. The bundle logic indicates it is tied to game-related
contexts and template families.

Practical rule:

- use `GameCTA` only in templates that already use it in the bundle
- for generic custom payloads, prefer ordinary `actions`


### 10. `userId` still matters even for local payloads

The overlay emits a client error when notifications arrive with unexpected
`userId` values such as invalid/system-user sentinels.

Practical rule:

- use a normal logged-in user where possible
- avoid invalid/system sentinel IDs unless the source path clearly expects them


## Field Type Reference

### Top-level fields

- `rawData`: object
- `createdDateTime`: string, ISO8601 UTC
- `updatedDateTime`: string, ISO8601 UTC
- `expirationDateTime`: string, ISO8601 UTC
- `localNotificationId`: string
- `notificationId`: string
- `offConsoleToastType`: string
- `state`: string
- `userId`: number
- `isAnonymous`: boolean
- `userIdBackup`: number
- `soundEffect`: string
- `isSummaryProhibited`: boolean
- `fromUser`: object
- `associatedTitle`: object
- `uploadData`: object
- `iduMode`: number

Observed timestamp examples:

`createdDateTime` is used for sorting:

`updatedDateTime` is requested from popup DB accessors:

`localNotificationId`, when used, must be non-empty:

### Special `userId` values

The bundle exposes these user constants:

- `InvalidUserId = -1`
- `SystemUserId = 255`
- `EveryoneUserId = 254`

There is also runtime state for:

- `loginUsers`
- `launchedUser`

Behavioral notes:

- normal popup visibility checks allow a toast when:
  - `userId === EveryoneUserId`, or
  - `userId` is present in `loginUsers`
- when there are no `loginUsers`, some `EveryoneUserId` notifications are
  suppressed if they depend on user/title metadata or local-notification sync
  paths
- ordinary notification validation treats `InvalidUserId` and
  `SystemUserId` as unexpected for normal local toast payloads

Practical rule:

- for ordinary local notifications, use a real logged-in user
- use `EveryoneUserId (254)` only if broad fan-out behavior is intentional
- avoid `InvalidUserId (-1)` and `SystemUserId (255)` for normal toast payloads
- avoid metadata-heavy `EveryoneUserId` payloads when no user is logged in


### `isAnonymous` and `userIdBackup`

`isAnonymous` is not just cosmetic. The bundle rewrites anonymous payloads so
they behave as broadcast-style notifications:

- `userId` is rewritten to `EveryoneUserId`
- original `userId` is preserved as `userIdBackup`

Anonymous payloads are then fanned out across `loginUsers` in popup/summary
dispatch paths.

Practical rule:

- use `isAnonymous` only when you intentionally want broader multi-user fan-out
- do not set `userIdBackup` manually unless you are reproducing an existing
  internal path


### `notificationId`, `localNotificationId`, and `offConsoleToastType`

These fields participate in remote/off-console propagation and cached
notification synchronization.

- `localNotificationId`
  - local identifier used before remote upload/sync
  - if present, must be non-empty
- `notificationId`
  - server/remote-side notification identifier
  - used in off-console post requests and remote completion flow
- `offConsoleToastType`
  - observed values:
    - `Always` (default when omitted in one suppression path)
    - `OnDemand`
  - no other values are clearly evidenced by the bundle scans used here
  - `OnDemand` changes suppression behavior:
    - if `notificationId` exists, payload is queued for off-console post
    - if `notificationId` does not exist yet but `localNotificationId` does,
      payload is held until local/remote sync completes

Practical rule:

- for plain local toasts, these fields are usually unnecessary
- use them only if you intentionally interact with notification DB sync or
  off-console delivery behavior


### `state`

`state` is present in notification records/models but is not part of the small
required `rawData` contract for local send.

Practical rule:

- local payload generators usually do not need to set `state`
- if present, treat it as notification record metadata rather than renderer
  configuration


### `associatedTitle`

`associatedTitle` is a top-level context object used heavily by game-centered
templates and placeholder hydration.

Observed/common shapes:

```json
{
  "npTitleId": "PPSA00000_00"
}
```

It is used by:

- title placeholders
- game CTA renderers
- game preparation / game-to-player / tournament-style payloads
- title metadata hydration paths

Practical rule:

- if a toast is game-centric, prefer providing `associatedTitle`
- the most important observed member is `npTitleId`


### `fromUser`

`fromUser` is a top-level user context object used by social templates and
placeholder expansion.

Observed/common shapes:

```json
{
  "accountId": "3184494961250237665"
}
```

It is used by:

- `%fromUser%` placeholder flows
- profile/avatar hydration
- friend/session/social notification renderers

Practical rule:

- if a toast is about another player, provide `fromUser`
- `accountId` is the most consistently observed field


### `uploadData`

`uploadData` is not renderer UI data. It is extra structured metadata attached
to some internally generated notifications, especially around game/download
preparation flows.

Observed shape family:

```json
{
  "serviceId": "downloadManager",
  "eventType": "gameReadyToPlay",
  "parameters": {
    "associatedTitle": "...",
    "contentType": "...",
    "titleId": "...",
    "entitlementId": "..."
  }
}
```

Practical rule:

- custom local toasts usually do not need `uploadData`
- only add it if you are intentionally mirroring an internal service-generated
  notification format


### `iduMode`

`iduMode` exists in the bundle and affects some display/expandability logic.
One observed rule is that a toast is not expandable when `iduMode === 1`.

Practical rule:

- omit `iduMode` unless you are reproducing a known IDU-specific path


### `soundEffect`

`soundEffect` is an optional top-level/model field used by the popup renderer.

Behavior:

- if `soundEffect` is present and not `"none"`, the renderer plays that sound ID
- if `soundEffect === "none"`, no sound is played
- if the field is absent, the bundle chooses a default based on context:
  - error-like channels get an error sound
  - interactive toasts get an interactive “something to do” sound
  - otherwise an informative “something to read” sound is used

Practical rule:

- omit `soundEffect` for default platform behavior
- use `"none"` if you explicitly want a silent toast


### `isSummaryProhibited`

`isSummaryProhibited` is a top-level/model flag used by summary aggregation
paths. Summary-mode code explicitly filters out notifications with this flag.

Practical rule:

- set `isSummaryProhibited: true` when a notification should remain standalone
  and not be aggregated into summary toasts
- omit it for normal summary-eligible notifications


### What is still only partially understood

The bundle now gives enough information to build most practical local toasts
reliably, but some fields are still only partially mapped:

- many `channelType` values are observed, but not all of their routing/display
  semantics are fully documented
- specialized `viewData.parameters` contracts vary by template and are not
  always validated in one central place
- `offConsoleToastType` is only partially visible through suppression/posting
  paths; the documented behavior is accurate for observed `OnDemand` and
  default-style handling, but not a full enum reference
- several scenario templates rely on async `preFetch` / hydrated runtime models,
  so a JSON example alone does not always capture the full runtime contract
- no explicit hard payload-size limit or max string-length contract has been
  identified in the JS bundle alone
- not every `useCaseId` found in samples/builders has a fully explained
  behavioral contract

### `rawData`

- `viewTemplateType`: string
- `channelType`: string
- `useCaseId`: string
- `bundleName`: string
- `toastOverwriteType`: string
- `isImmediate`: boolean
- `priority`: number
- `viewData`: object
- `platformViews`: object
- `platformParameters`: object

### `priority`

For local/raw notification payloads, the bundle samples and builders commonly
use numeric priorities such as:

- `0`
- `1`
- `2`

Elsewhere in the broader notification stack there are also symbolic priorities
like `Low`, `Normal`, `High`, and `Legal`, but those belong to other internal
models and dispatcher layers rather than the simple local `rawData` examples.

Practical rule:

- for local JSON payloads, prefer the observed numeric values
- if you are copying a concrete builder/sample, keep its priority unchanged


### `viewData`

- `icon`: object
- `icons`: array
- `message`: object
- `subMessage`: object
- `extraMessage`: object
- `preMessage`: object
- `actions`: array
- `parameters`: object

### Text object shape

`message`, `subMessage`, `extraMessage`, and `preMessage` use the same general
shape:

```json
{
  "body": "Text or %placeholder%",
  "placeHolders": { ... },
  "optionalProperties": ["userGeneratedContent"]
}
```

Observed placeholder `objectType` values in the bundle:

- strongly supported in generic/title paths:
  - `User`
  - `Title`
  - `ConceptByTitle`
  - `TitleByTitle`
- additionally visible in sample catalogues:
  - `String`
  - `Number`

Practical rule:

- `String` and `Number` are good for local sample-like payloads
- for full renderer/prefetch compatibility, the safest types remain
  `User`, `Title`, `ConceptByTitle`, and `TitleByTitle`

### Accessibility props

The bundle supports:

- `messageAccessibilityProps`
- `subMessageAccessibilityProps`
- `extraMessageAccessibilityProps`

Observed members:

- `accessibilitySpeakable`
- `accessibilityLabel`
- `accessibilityHint`
- `accessibilityWidgetType`
- `accessibilityCustomSpeechOrder`


## Common `rawData` Fields

Frequently seen fields:

- `viewTemplateType`
- `channelType`
- `useCaseId`
- `bundleName`
- `toastOverwriteType`
- `isImmediate`
- `priority`
- `viewData`
- `platformViews`
- `platformParameters`

### `toastOverwriteType`

Observed values:

- `No`
- `InQueue`
- `Always`

The overlay uses replacement rules based on:

1. same `id`
2. same `bundleName`
3. same `useCaseId` if no `bundleName`


`platformParameters.toastOverwriteType` can override the root value.


### `platformParameters`

Only one member is clearly confirmed by the generic local toast path:

- `platformParameters.toastOverwriteType`

Observed behavior:

- when present, it overrides the root `toastOverwriteType` during replacement
  checks
- no other `platformParameters` members are clearly consumed in the same
  generic renderer / replacement path

Practical rule:

- for custom payloads, treat `platformParameters` as effectively undocumented
  except for `toastOverwriteType`
- avoid inventing extra members unless you are copying a concrete internal
  builder


### `bundleName`

`bundleName` is used as a grouping/replacement key and also appears in listener
registration/filtering APIs.


### `bundleNameFlag`

`bundleNameFlag` appears in popup DB / accessor plumbing rather than in the
small local `rawData` examples.

Observed behavior:

- popup accessors default it to `0` when omitted
- it is forwarded into `NotificationDb2PopupAccessor.getItems(...)`
- it affects which grouped popup items are fetched from cached notification DB
  state

Practical rule:

- treat `bundleNameFlag` as internal popup-cache/query metadata
- do not add it to custom local payloads unless you are intentionally copying a
  DB-backed accessor path


### `useCaseId`

`useCaseId` drives:

- validation
- replacement behavior
- suppression/in-context filtering
- template-specific rendering/behavior


### Observed special `useCaseId` behavior

Some `useCaseId` values are treated as more than just labels.

- `IN_CONTEXT`
  - appears in popup-suppressed listener examples
  - can be watched via notification listeners scoped by `useCaseId`
  - used together with `channelType: "InContext"` in debug samples
- `NO_SUPPRESS`
  - appears in debug/suppression samples as a way to test non-suppressed
    in-context behavior
  - treat this as observed/debug behavior, not a guaranteed public contract
- `POWER_MANAGEMENT`
  - used by power-related system sample payloads
- `DISC_LOADING`
  - used by disc-loading related sample payloads
- `NUCxxx`
  - many specialized templates are keyed primarily by `NUC...` values
  - the most important family is `InteractiveToastGamePreparation`

Practical rule:

- for generic custom toasts, any stable string may satisfy structure checks, but
  platform behavior becomes more predictable when using a known/observed
  `useCaseId`
- for specialized templates, copy the exact `useCaseId` family used by the
  bundle builder


### Confirmed `useCaseId` catalogue

The following catalogue contains only associations that are directly confirmed
by dedicated builders or by explicit scenario/sample families.

- `InteractiveToastGamePreparation`
  - `NUC238` -> `gameReadyToPlay`
  - `NUC241` -> `dlcReadyToPlay`
  - `NUC253` -> `insufficientSystemStorage`
  - `NUC255` -> `insufficientExtendedStorage`
  - `NUC257` -> `downloadFailed`
  - `NUC478` -> `insufficientM2Storage`
  - `NUC547` -> `addonsReadyToPlay`
- `InteractiveToastMessages`
  - `NUC63`, `NUC181` -> add group
  - `NUC64`, `NUC182` -> join group
  - `NUC65`, `NUC183` -> multi-user join group
  - `NUC69`, `NUC174` -> message reply
  - `NUC108`, `NUC179` -> sticker
  - `NUC109` -> voice message
  - `NUC110` -> image
  - `NUC111` -> music
  - `NUC112` -> video
  - `NUC116` -> invitation / user scheduled event
  - `NUC117` -> official event
  - `NUC351`, `NUC352`, `NUC353` -> image + text
  - `NUC354`, `NUC355`, `NUC356` -> voice + text
- `InteractiveToastVoiceChat`
  - `NUC66` -> party started with you
  - `NUC185` -> private party start / invitation
  - `NUC527` -> open party invite
  - `NUC528` -> open party invite with screen share
  - `NUC532` -> request to join
- `InteractiveToastTournaments`
  - `NUC510` -> tournament started
  - `NUC511` -> join match
  - `NUC512` -> round win
  - `NUC513` -> next round ready
  - `NUC514` -> tournament lost
  - `NUC515` -> tournament won
  - `NUC516` -> tournament cancelled
- `InteractiveToastAllowListRequest`
  - `NUC469`, `NUC473` -> deep-link CTA
  - `NUC470`, `NUC474` -> `GameCTA`
  - `NUC471`, `NUC472`, `NUC475`, `NUC476` -> `OK` button only
- `InteractiveToastActivityChallenges`
  - `NUC264` -> reclaim the lead
  - `NUC265` -> challenge completed
  - `NUC269` -> fell out of top 100
  - `NUC271` -> final global placement
  - `NUC272` -> challenge ended
  - `NUC345` -> direct challenge from another user
  - `NUC463` -> personal best
- `InteractiveToastFriendRequest`
  - `NUC1` -> standard / close-friend request variants
  - `NUC2` -> real-name / close-friends wording
- `InteractiveToastPlayerSessionRequestToJoin`
  - `NUC449` -> request to join player session
- `InteractiveToastTrophy`
  - `NUC55` -> trophy family
  - `NUC273` -> trophy family
- `InteractiveToastScreenShare`
  - `NUC104` -> dedicated screen-share interactive template
  - `NUC171`, `NUC176`, `NUC177`, `NUC178`, `NUC359`, `NUC361`, `NUC378`,
    `NUC379`, `NUC450`, `NUC453` -> screen-share related builder-visible cases
- `InteractiveToastSharePlay`
  - `NUC107` -> dedicated Share Play interactive template
  - `NUC132`, `NUC133`, `NUC135`, `NUC136`, `NUC137`, `NUC138`, `NUC139`,
    `NUC140`, `NUC141`, `NUC142`, `NUC143`, `NUC145`, `NUC147`, `NUC148`,
    `NUC149`, `NUC150`, `NUC151`, `NUC152`, `NUC154`, `NUC155`, `NUC156`,
    `NUC158`, `NUC160`, `NUC162`, `NUC168`, `NUC170`, `NUC362`, `NUC363`,
    `NUC380`, `NUC381`, `NUC382`, `NUC383`, `NUC384`, `NUC385`, `NUC386`,
    `NUC407`, `NUC415`, `NUC436` -> Share Play builder-visible cases
- `InteractiveToastSaveDataMessage`
  - `NUC430`, `NUC431`, `NUC433`, `NUC461` -> dedicated save-data template
  - `NUC563`, `NUC564`, `NUC565`, `NUC566` -> same family but routed to
    `ToastTemplateB`
- `InteractiveToastPSNowPlayer`
  - `NUC192` -> dedicated PS Now / queue-turn template
  - `NUC190`, `NUC280` -> related family routed to `InteractiveToastTemplateB`
  - `NUC191` -> related family routed to `ToastTemplateB`
- `InteractiveToastSuppressionOnboarding`
  - `NUC411` -> suppression-onboarding flow
- `PlayStationSafety` / `IconOnlyToast`
  - `NUC283` -> signed-out / safety flow
- `VolumeIndicator` / `MixSliderIndicator`
  - `NUC58` -> volume / mix slider family
- broadcast / sharing informative family
  - `NUC10` -> broadcast paused / blocked-scene family
  - `NUC11` -> broadcasting resumed family
  - `NUC12` -> broadcast stopped family
  - `NUC437` -> now broadcasting family
- simple feedback families
  - `NUC459` -> request-sent feedback
  - `NUC460` -> request-send-error feedback
- indicator / system families with explicit `sendById` coverage
  - `NUC57` -> device connection indicator family
  - `NUC58` -> volume / mix slider family
  - `NUC376` -> crash-report family


### `channelType`

`channelType` is not just visual categorization. The bundle uses it in:

- DND suppression
- popup settings / category filters
- focus-mode filters
- default sound selection
- some template-specific routing

Observed `channelType` values in samples and builders include:

- `Downloads`
- `Uploads`
- `SystemFeedback`
- `SystemError`
- `ServiceFeedback`
- `ServiceError`
- `Trophies`
- `ActivityChallenges`
- `FriendRequest`
- `GameInvites`
- `Party`
- `Messages`
- `Tournaments`
- `PlayStationSafety`
- `InContext`
- `Displayable`
- `FromPlayStation`
- `PlayStationPlus`
- `PlayStationStore`
- `PlayStationNow`
- `PlayStationMusic`
- `PlayStationSeasonPass`
- `EAAccess`
- `EaPlay`
- `GameContentAnnouncement`
- `WhenFriendsGoOnline`
- `WishlistItems`
- `MusicTrackChange`
- `FamilyActivity`
- `Accolades`
- `gameVr:Informative`
- `Test`
- `testGroup1`

Behavioral notes:

- some channels are exempt from DND/suppress rules:
  - `SystemFeedback`
  - `ServiceFeedback`
  - `SystemError`
  - `ServiceError`
- settings/focus filters explicitly switch on many user-facing channel names
  such as `GameInvites`, `Trophies`, `Messages`, `Party`, `Downloads`,
  `Uploads`, `Tournaments`, and subscription/store families

Observed settings-category groupings:

- gaming:
  - `GameInvites`
  - `Trophies`
  - `ActivityChallenges`
  - `GameContentAnnouncement`
  - `Tournaments`
- social:
  - `FriendRequest`
  - `WhenFriendsGoOnline`
  - `Messages`
  - `Party`
  - `Accolades`
  - `FamilyActivity`
- media/downloads:
  - `MusicTrackChange`
  - `Downloads`
  - `Uploads`
- account/offers/subscriptions:
  - `WishlistItems`
  - `FromPlayStation`
  - `PlayStationPlus`
  - `PlayStationStore`
  - `PlayStationNow`
  - `PlayStationMusic`
  - `PlayStationSeasonPass`
  - `EAAccess`
  - `EaPlay`

Practical rule:

- do not treat `channelType` as arbitrary free text
- for custom payloads, prefer already-observed values
- if you want system-style behavior that is less likely to be filtered, use a
  channel that matches the template family you are imitating


### Confirmed `channelType` semantics

The following behavior is directly confirmed by popup filtering and suppression
logic.

- DND suppression explicitly does not suppress:
  - `SystemFeedback`
  - `ServiceFeedback`
  - `SystemError`
  - `ServiceError`
- Popup settings explicitly bypass the global `allowPopupNotifications` gate
  for:
  - `FamilyActivity`
  - `PlayStationSafety`
  - `SystemFeedback`
  - `ServiceFeedback`
  - `SystemError`
  - `ServiceError`
- Focus-mode filtering explicitly branches on these channel families:
  - `GameInvites`
  - `Trophies`
  - `GameContentAnnouncement`
  - `ActivityChallenges`
  - `Party`
  - `Accolades`
  - `FriendRequest`
  - `WhenFriendsGoOnline`
  - `Messages`
  - `MusicTrackChange`
  - `Downloads`
  - `Uploads`
  - `WishlistItems`
  - `FromPlayStation`
  - `Tournaments`
  - `PlayStationPlus`
  - `PlayStationStore`
  - `PlayStationNow`
  - `PlayStationMusic`
  - `PlayStationSeasonPass`
  - `EAAccess`
  - `EaPlay`
- Popup settings map channels into these user-facing groups:
  - gaming:
    - `GameInvites`
    - `Trophies`
    - `ActivityChallenges`
    - `GameContentAnnouncement`
    - `Tournaments`
  - social:
    - `FriendRequest`
    - `WhenFriendsGoOnline`
    - `Messages`
    - `Party`
    - `Accolades`
  - media / downloads:
    - `MusicTrackChange`
    - `Downloads`
    - `Uploads`
  - account / offers / subscriptions:
    - `WishlistItems`
    - `FromPlayStation`
    - `PlayStationPlus`
    - `PlayStationStore`
    - `PlayStationNow`
    - `PlayStationMusic`
    - `PlayStationSeasonPass`
    - `EAAccess`
    - `EaPlay`
- The following channel strings are confirmed only as observed sample/debug
  values. No stronger display or suppression contract is asserted here:
  - `Test`
  - `testGroup1`
- `InContext`
  - used by explicit in-context suppression/debug samples together with
    `useCaseId: "IN_CONTEXT"` and `useCaseId: "NO_SUPPRESS"`
  - also used with `Notification.sendToApplication(...)` debug paths
- `Displayable`
  - used by explicit “displayable notification” debug samples for both
    informative and interactive payloads
  - confirmed in both logged and non-logged local send examples
- `gameVr:Informative`
  - used as the channel and bundle family for VR-specific informative sample
    payloads
  - sample family pairs it with `platformViews.virtualReality3D`


### `isImmediate`

Observed as a boolean on both informative and interactive payloads.

The bundle’s sample set includes both:

- `isImmediate: true`
- `isImmediate: false`


## `viewData` in More Detail

The renderer reads `viewData` and optionally replaces it with a platform-specific
variant.

Common members:

- `icon`
- `icons`
- `message`
- `subMessage`
- `extraMessage`
- `preMessage`
- `actions`
- `parameters`


### Text order

For screen-reader flow:

- `TemplateB` uses `subMessage -> message -> extraMessage`
- other templates use `message -> subMessage -> extraMessage`


### `preMessage`

`preMessage` exists in the bundle and is processed in notification/accessibility
paths. It is not used by the common `InteractiveToastTemplateA/B` samples, but
it is supported in the model.


### `viewData.parameters`

`viewData.parameters` is the least standardized part of the payload model.
There is no single global schema; instead, specialized templates read different
keys.

Recurring parameter families visible in the bundle:

- `expandedType`
  - switches expanded-body renderer variants
  - heavily used by `InteractiveToastGamePreparation`
- `actionProps`
  - embedded action object used by expanded game-preparation layouts
  - commonly mirrors a `DeepLink` action
- `titleId`
  - game/content context for CTA or navigation
- `contentId`
  - downloadable-content identifier for some download-manager scenarios
- `errorCode`
  - shown in failure/storage/problem flows
- `focusMode`
  - used by in-context / settings-adjustment paths
- `groupId`
  - party/message/group context
- `messageUid`
  - system-message or messaging lookup key
- `activityId`
  - activity-challenge related context
- `occurrenceId`
  - tournament occurrence context
- `associatedTitle`
  - title context passed through specialized builders
- `fromUser`
  - user context passed through specialized builders
- `text`
  - generic text payload used in some summary/system variants

Practical rule:

- do not assume `viewData.parameters` is generic across templates
- copy the exact parameter shape from a known builder/sample for the target
  template
- if you are building custom generic `TemplateA/B` notifications, you often do
  not need `parameters` at all


### Confirmed template-specific parameter consumption

This subsection lists only parameters that are directly read by template code,
or that recur consistently across the dedicated sample family for that
template.

- `InteractiveToastGamePreparation`
  - directly consumed:
    - `viewData.parameters.expandedType`
    - `viewData.parameters.titleId`
  - confirmed additional builder-populated fields for download/game-preparation
    scenarios:
    - `viewData.parameters.actionProps`
    - `viewData.parameters.errorCode`
    - `viewData.parameters.contentId`
- `InteractiveToastGameToPlayer`
  - directly consumed:
    - `viewData.parameters.activityId`
  - top-level context read together with it:
    - `associatedTitle.npTitleId`
  - the allow-list CTA family additionally reads:
    - `viewData.parameters.fromUser`
    - `viewData.parameters.associatedTitle`
- `InteractiveToastSystemMessage`
  - directly consumed:
    - `viewData.parameters.messageUid`
- `InteractiveToastMessages`
  - consistently present across the dedicated message samples:
    - `viewData.parameters.groupId`
    - `viewData.parameters.threadId`
  - confirmed recurring scenario-specific fields:
    - `viewData.parameters.messageUid`
    - `viewData.parameters.joinedUsers`
    - `viewData.parameters.groupType`
    - `viewData.parameters.fromUser`
- `InteractiveToastVoiceChat`
  - confirmed recurring sample fields:
    - `viewData.parameters.sessionId`
    - `viewData.parameters.groupId`
    - `viewData.parameters.fromUser`
- `InteractiveToastTournaments`
  - confirmed recurring sample fields:
    - `viewData.parameters.occurrenceId`
    - `viewData.parameters.pairingId`
    - `viewData.parameters.remainingDuration`
- `InteractiveToastTemplateA`
  - no template-specific `viewData.parameters` contract is confirmed here
  - dedicated samples often omit `parameters` entirely
- `InteractiveToastTemplateB`
  - no template-specific `viewData.parameters` contract is confirmed here
  - specialized scenario families that reuse TemplateB may still define their
    own parameters


## Icon Formats

Observed icon types:

- `FromUser`
- `AssociatedTitle`
- `ConceptByTitle`
- `TitleByTitle`
- `Predefined`
- `Url`
- `DeviceInfo`

Examples are visible in the bundle’s sample payloads:

### Example: predefined icon

```json
"icon": {
  "type": "Predefined",
  "parameters": {
    "icon": "system"
  }
}
```

### Example: URL icon

```json
"icon": {
  "type": "Url",
  "parameters": {
    "url": "https://...",
    "iconSize": "Square"
  }
}
```

### Example: title-derived icon

```json
"icon": {
  "type": "TitleByTitle",
  "parameters": {
    "npTitleIds": ["CUSL00217_00"]
  }
}
```

### Example: device info

```json
{
  "type": "DeviceInfo",
  "parameters": {
    "device": "game",
    "battery": "battery_empty"
  }
}
```

### Icon notes

- `icons` can be an array, not only a single `icon`.
- sample payloads show `disabled: true` on predefined icons.
- sample payloads show `iconSize` values such as `Square` and `Landscape`.
- title/user-derived icon types trigger metadata fetch behavior.


### `icon.type` contracts

For practical payload generation, these are the most important observed icon
contracts:

- `FromUser`
  - simplest working shape uses top-level `fromUser`
  - sample:
    - `fromUser: { accountId: "..." }`
    - `viewData.icon: { type: "FromUser" }`
- `AssociatedTitle`
  - simplest working shape uses top-level `associatedTitle`
  - sample:
    - `associatedTitle: { npTitleId: "..." }`
    - `viewData.icon: { type: "AssociatedTitle" }`
- `ConceptByTitle`
  - uses `viewData.icon.parameters.npTitleIds`
- `TitleByTitle`
  - uses `viewData.icon.parameters.npTitleIds`
- `Predefined`
  - uses `viewData.icon.parameters.icon`
- `Url`
  - uses `viewData.icon.parameters.url`
  - optional `iconSize` values observed:
    - `Square`
    - `Landscape`
- `DeviceInfo`
  - used by `DeviceConnectionIndicator` / VR indicator-like paths
  - carries device-specific parameters rather than title/user metadata

Practical rule:

- prefer the shortest working form shown in bundle samples
- for `FromUser` and `AssociatedTitle`, top-level context objects are the most
  reliable observed source
- for `ConceptByTitle` / `TitleByTitle`, pass `npTitleIds`


### Confirmed `Predefined` icon names

The following `Predefined` icon names are directly observed in working sample
payloads or concrete builder paths and are therefore safe choices for local
payload generation:

- `community`
- `download`
- `error_message_caution`
- `family`
- `headset`
- `localasset_system_software_default`
- `messages`
- `mic`
- `mic_mute_status`
- `no_operation_allowed`
- `notification_off`
- `people`
- `photo_storing_done`
- `ps4`
- `ps_user`
- `share_play`
- `share_screen`
- `sound_level_game_up_party_down`
- `sound_level_party_up_game_down`
- `sound_speaking`
- `system`
- `trophies`
- `users_guide_helpful_info`
- `voice_command`

Practical rule:

- if you want maximum compatibility, prefer icons from this list
- `system`, `download`, `messages`, `ps_user`, and `sound_speaking` appear
  especially often in sample and builder paths


### Confirmed `DeviceInfo` parameter values

Only a small number of concrete `DeviceInfo.parameters.device` values are
directly confirmed by sample payloads and builder-generated payloads:

- `game`
- `psmove`

Observed companion parameters for `DeviceInfo`:

- `battery`
  - confirmed values observed in payloads:
    - `battery_empty`
    - `battery_low`

Practical rule:

- treat `DeviceInfo` as a specialized path used mainly by
  `DeviceConnectionIndicator` and related device-oriented templates
- use only the confirmed `device` values above unless runtime testing confirms
  additional values on console


### Observed device and hardware-related icon ids

The bundle also contains a much larger icon-id catalogue used by general UI
components and device-oriented notification builders. The following icon ids are
observed and are useful as a reference when looking for device- or
hardware-themed visuals. These are observed icon resource ids, not all of them
are confirmed as accepted `Predefined` toast icons.

Audio, microphone, and headset:

- `headphone`
- `headset`
- `mic`
- `mic_disconnected`
- `mic_error`
- `mic_mute`
- `mic_mute_status`
- `mic_preference`
- `mic_speaking`
- `ps5_wireless_headset`
- `singstar_mic`
- `sound_all`
- `sound_error`
- `sound_input`
- `sound_level_game_up_party_down`
- `sound_level_party_up_game_down`
- `sound_mute`
- `sound_mute_status`
- `sound_output`
- `sound_party`
- `sound_preference`
- `sound_screen`
- `sound_speaking`

Controllers, input, and general devices:

- `devices`
- `ds4_connected_via_usb`
- `game`
- `game_controller_1`
- `game_controller_2`
- `game_controller_3`
- `game_controller_4`
- `headset`
- `human_interface_device`
- `keyboard`
- `mouse`
- `mouse_keyboard`
- `ps5_media_remote`
- `ps_button`
- `pscamera`
- `psmove`
- `psmove_double`
- `psmove_pscamera`
- `remote_controller`

PS VR / VR-related:

- `psvr`
- `psvr2`
- `psvr2_adjust_visibility`
- `psvr2_eye_tracking`
- `psvr2_headset_motion_controller`
- `psvr2_headset_motion_controller_vibration`
- `psvr2_headset_vibration`
- `psvr2_motion_controller`
- `psvr2_motion_controller_left`
- `psvr2_motion_controller_right`
- `psvr2_motion_controller_trigger_effect`
- `psvr2_motion_controller_trigger_effect_vibration`
- `psvr2_motion_controller_vibration`
- `psvr2_playermode`
- `psvr2_playermode_roomscale`
- `psvr2_playermode_sitting`
- `psvr2_playermode_standing`
- `psvr2_power`
- `psvr2_rumble_off`
- `psvr2_rumble_on`
- `psvr2_set_play_area`
- `psvr_adjust_vr_headset_position`
- `psvr_and_psvr2`
- `psvr_aimcontroller`
- `psvr_aimcontroller_pscamera`
- `psvr_confirm_your_position`
- `psvr_power`
- `psvr_pscamera`
- `psvr_pscamera_psmove`
- `psvr_reset_screen_mode`
- `psvr_screen_brightness`
- `psvr_screen_size`

Connection, remote play, and external devices:

- `camera_disconnected`
- `camera_generic`
- `camera_mute`
- `camera_mute_status`
- `camera_preference`
- `connection_to_ps4_connected`
- `connection_to_ps4_not_connected`
- `connection_to_ps4_unstable`
- `ethernet`
- `ethernet_disconnected_status`
- `ethernet_error`
- `network`
- `network_caution`
- `network_connection_test`
- `network_disconnected`
- `network_disconnected_status`
- `network_error`
- `phone`
- `remote_play_blocked_scene`
- `remote_play_connection`
- `remote_play_disconnected_status`
- `remote_play_exit`
- `remote_play_stop`
- `smartphone`
- `smartphone_tablet`

Practical rule:

- for toast payloads, prefer the confirmed `Predefined` list above
- use this larger catalogue as a hint for promising icon ids when reverse
  engineering additional device-oriented templates or testing on-console


## Text Placeholders and Object Types

Typical placeholder usage:

```json
"subMessage": {
  "body": "%titleByTitle%",
  "placeHolders": {
    "titleByTitle": {
      "objectType": "TitleByTitle",
      "npTitleIds": ["CUSL00217_00"],
      "defaultValue": "default title name"
    }
  }
}
```

Observed placeholder object types:

- `User`
- `Title`
- `ConceptByTitle`
- `TitleByTitle`
- `String`
- `Number`

The bundle’s metadata logic specifically recognizes:

- `Title`
- `ConceptByTitle`
- `TitleByTitle`


### `optionalProperties`

The bundle sample payloads show:

- `optionalProperties: ["userGeneratedContent"]`


## Actions

Interactive toasts can carry `actions`.

Observed action types:

- `DeepLink`
- `ActionCardLink`
- `PlayerControl`

### Example: DeepLink

```json
{
  "actionName": "Go to Profile",
  "actionType": "DeepLink",
  "defaultFocus": true,
  "parameters": {
    "actionUrl": "pspr:show"
  }
}
```

### Example: ActionCardLink

```json
{
  "actionName": "Go to Screen Share AC",
  "actionType": "ActionCardLink",
  "parameters": {
    "type": "ActionCardScreenShare",
    "param": {}
  }
}
```

### Example: PlayerControl

```json
{
  "actionType": "PlayerControl",
  "actionName": "Cancel",
  "parameters": {
    "command": "CancelGame"
  }
}
```


### Action notes

- `defaultFocus: true` is supported.
- `DeepLink` actions typically use `parameters.actionUrl`.
- `ActionCardLink` actions use `parameters.type` and `parameters.param`.
- `PlayerControl` uses command-style parameters, but it is not handled by the
  generic action factory the same way `DeepLink` and `ActionCardLink` are.

The bundle also contains internal focus target names used by the UI:

- `defaultFocus`
- `defaultFocusLargeText`
- `defaultFocusLargeTextMultiList`
- `GameCTAButton`

These are internal focus model names, not fields you set directly in payloads.


### Generic action contracts

For custom generic interactive toasts, the bundle-backed contracts are:

- `DeepLink`
  - required in practice:
    - `actionType: "DeepLink"`
    - `parameters.actionUrl` or `parameters.url`
  - behavior:
    - checkout URLs are routed specially
    - other URLs go through `LinkingPS.openURLArg(...)`
- `ActionCardLink`
  - required in practice:
    - `actionType: "ActionCardLink"`
    - `parameters.type`
    - `parameters.param`
- `PlayerControl`
  - not part of the generic action factory path
  - use only in templates that already do so in the bundle

Practical rule:

- for custom local payloads, `DeepLink` is the safest action type
- `ActionCardLink` is valid, but should follow sample structures closely


## Platform-Specific Views

### `platformViews.previewDisabled`

Interactive toasts can define a simplified preview-disabled variant:

```json
"platformViews": {
  "previewDisabled": {
    "viewData": {
      "icon": { ... },
      "message": { "body": "New Message" }
    }
  }
}
```

The bundle explicitly indicates that preview-disabled handling applies to
interactive toasts, not informative ones.


### `platformViews.virtualReality3D`

VR-specific alternate views exist with `toastHeaderTemplate` values such as:

- `VrTemplateA`
- `VrTemplateB`
- `VrTemplateC`
- `VrTemplateD`
- `VrTemplateE`

Source samples:

### `platformViews.virtualReality3D2`

The bundle also processes `virtualReality3D2` alongside `virtualReality3D`.
Some builders generate both variants from the same base data.


### `platformViews.accessibility`

The bundle also processes an accessibility-specific alternate view:

```json
"platformViews": {
  "accessibility": {
    "viewData": { ... }
  }
}
```


Behavior note:

- when present, `platformViews.accessibility.viewData` replaces normal
  `viewData` for the accessibility-specific render path rather than being
  shallow-merged into it

Practical rule:

- treat `platformViews.accessibility.viewData` as a complete alternate payload
  for that path

### Platform-view selection order

The bundle does not treat all alternate views equally.

Observed order in the relevant selection paths:

- for ordinary popup rendering:
  - `virtualReality3D` wins in VR mode
  - otherwise `previewDisabled` can replace base `viewData`
  - otherwise base `viewData` is used
- in accessibility-specific rendering paths:
  - `platformViews.accessibility.viewData` overrides normal `viewData`
  - in some paths it also takes precedence over `previewDisabled`

Practical rule:

- do not expect `previewDisabled` to be the final override in all modes
- treat accessibility and VR alternate views as higher-priority render-path
  substitutions


## Exported Template Families

The bundle exports at least these template components:

- `InteractiveToast`
- `InteractiveToastTemplateA`
- `InteractiveToastTemplateB`
- `Toast`
- `ToastTemplateA`
- `ToastTemplateB`
- `HandoverIndicator`
- `DeviceConnectionIndicator`
- `InteractiveToastMessages`
- `InteractiveToastTrophy`
- `InteractiveToastPlayerSessionInvitation`
- `InteractiveToastLegacySessionInvitation`
- `InteractiveToastScreenShare`
- `InteractiveToastSharePlay`
- `InteractiveToastSummary`
- `InteractiveToastFriendRequest`
- `InteractiveToastGamePreparation`
- `IconOnlyToast`
- `VolumeIndicator`
- `MixSliderIndicator`
- `InteractiveToastFriendAvailable`
- `InteractiveToastActivityChallenges`
- `InteractiveToastGameToPlayer`
- `InteractiveToastVoiceChat`
- `InteractiveToastSystemMessage`
- `InteractiveToastSuppressionOnboarding`
- `InteractiveToastPSNowPlayer`
- `InteractiveToastSaveDataMessage`
- `InteractiveToastPlayerSessionRequestToJoin`
- `InteractiveToastAllowListRequest`
- `InteractiveToastVoiceCommandUserFeedback`
- `TransitionSample`


## Minimal Working Shapes for Common Templates

These are not the only valid payloads, but they summarize the smallest
practical shapes that are strongly supported by bundle samples.

### `ToastTemplateA`

- required in practice:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.message`
- commonly added:
  - `viewData.icon`

Representative shape:

```json
{
  "rawData": {
    "viewTemplateType": "ToastTemplateA",
    "useCaseId": "NUC_SAMPLE",
    "channelType": "SystemFeedback",
    "viewData": {
      "message": { "body": "PrimaryContent" }
    }
  }
}
```

### `ToastTemplateB`

- required in practice:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.message`
- commonly added:
  - `viewData.subMessage`
  - `viewData.icon`

### `InteractiveToastTemplateA`

- required in practice:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.message`
  - at least one supported `action`
- commonly added:
  - `viewData.icon`

### `InteractiveToastTemplateB`

- required in practice:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.message`
- commonly added:
  - `viewData.subMessage`
  - `viewData.icon`
  - `actions`

### `InteractiveToastGamePreparation`

- required in practice:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - `viewData.parameters.expandedType`
  - `viewData.parameters.titleId`

### `IconOnlyToast`

- required in practice:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.icon`

### `DeviceConnectionIndicator`

- required in practice:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.devices`
  - in bundle samples/builders this uses `DeviceInfo`

### `HandoverIndicator`

- required in practice:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.icons`
  - `viewData.message`
  - `viewData.supportingIcons`

### `VolumeIndicator`

- required in practice:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.icon`
  - `viewData.contentType`
  - `viewData.progress.rate`

### `MixSliderIndicator`

- required in practice:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.leftIcon`
  - `viewData.rightIcon`
  - `viewData.value`

### `InteractiveToastTrophy`

- confirmed builder minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `bundleName`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - `viewData.parameters.useCaseId`
  - `viewData.parameters.trophy`
  - `viewData.parameters.game`
  - `viewData.parameters.label`
  - at least one `DeepLink` action

### `InteractiveToastPlayerSessionInvitation`

- confirmed sample minimum:
  - `viewTemplateType`
  - `associatedTitle`
  - `fromUser`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - `viewData.parameters.sessionId`
  - `viewData.parameters.groupId`
  - `viewData.parameters.associatedTitle`
  - `viewData.parameters.associatedTitles`

### `InteractiveToastLegacySessionInvitation`

- confirmed sample minimum:
  - `viewTemplateType`
  - `associatedTitle`
  - `fromUser`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - `viewData.parameters.invitationId`
  - `viewData.parameters.associatedTitle`
  - `viewData.parameters.associatedTitles`

### `InteractiveToastPlayerSessionRequestToJoin`

- confirmed sample minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `fromUser`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - `viewData.parameters.associatedTitles`
  - `viewData.parameters.fromUser`
  - `viewData.parameters.sessionId`
  - `viewData.parameters.sessionName`

### `InteractiveToastFriendRequest`

- confirmed sample minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `fromUser`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - `viewData.parameters.fromUser`
  - `viewData.parameters.fromUsers`
  - `viewData.parameters.totalCount`
  - `viewData.parameters.usersCount`

### `InteractiveToastFriendAvailable`

- confirmed sample minimum:
  - `viewTemplateType`
  - `fromUser`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`

### `InteractiveToastActivityChallenges`

- confirmed sample minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `associatedTitle`
  - `fromUser`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - `viewData.parameters.npCommunicationId`
  - `viewData.parameters.challengeName`
  - `viewData.parameters.runId`
  - `viewData.parameters.challengeId`

### `InteractiveToastMessages`

- confirmed sample minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `fromUser`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - `viewData.parameters.groupId`
  - `viewData.parameters.threadId`

### `InteractiveToastGameToPlayer`

- confirmed renderer minimum:
  - `viewTemplateType`
  - `associatedTitle`
  - `viewData.icon`
  - `viewData.subMessage`
  - `viewData.parameters.activityId`

### `InteractiveToastVoiceChat`

- confirmed sample minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `fromUser`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - at least one of:
    - `viewData.parameters.groupId`
    - `viewData.parameters.sessionId`

### `InteractiveToastSystemMessage`

- confirmed sample minimum:
  - `viewTemplateType`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - `viewData.parameters.messageUid`

### `InteractiveToastAllowListRequest`

- confirmed renderer minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `userId`
  - `viewData.parameters`
  - plus one of the following CTA-specific fields depending on `useCaseId`:
    - `viewData.parameters.fromUser`
    - `viewData.parameters.associatedTitle`

### `InteractiveToastTournaments`

- confirmed sample minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `associatedTitle`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - `viewData.parameters.occurrenceId`

### `InteractiveToastScreenShare`

- confirmed builder/sample minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `fromUser`
  - `viewData.icon`
  - `viewData.message`
  - for the dedicated interactive path, an `ActionCardLink` action with
    `type: "ActionCardScreenShare"`

### `InteractiveToastSharePlay`

- confirmed builder minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.icon`
  - `viewData.message`
  - template-specific actions vary by use case

### `InteractiveToastSummary`

- confirmed generated summary minimum:
  - `viewTemplateType`
  - `useCaseId: "NUCSUMMARY"`
  - `toastOverwriteType: "InQueue"`
  - `viewData.parameters.sourcePayloads`

### `InteractiveToastSaveDataMessage`

- confirmed builder minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - when details are available, a `DeepLink` action

### `InteractiveToastPSNowPlayer`

- confirmed builder minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - for actionable variants, one or more actions
    - `DeepLink`
    - optionally `PlayerControl`

### `InteractiveToastSuppressionOnboarding`

- confirmed sample/builder minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `isSummaryProhibited`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - `viewData.parameters.focusMode`

### `InteractiveToastVoiceCommandUserFeedback`

- confirmed builder minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData.icon`
  - `viewData.message`
  - `viewData.subMessage`
  - `viewData.parameters.useCaseId`
  - `viewData.parameters.agentIntentSessionId`
  - `viewData.parameters.voiceLanguage`
  - `viewData.parameters.label`

### `TransitionSample`

- confirmed sample minimum:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData`

Practical rule:

- start from these minimal shapes when building custom payloads
- add metadata and alternate platform views only after the base form renders

## Export Coverage From `72896+`

This section cross-checks every exported template in the bundle export list
around `72896+` and summarizes what is actually visible elsewhere in the file:
wrapper/renderer role, builder logic, sample payloads, and notable fields.

### Wrapper / base renderer exports

- `InteractiveToast`
  - appears to be the interactive card renderer/container selected by the
    overlay render path
  - not typically used as a payload `viewTemplateType`
  - render path uses template names such as `TemplateA`, `TemplateB`,
    `Toast`, `ToastOnSummaryToast`

- `Toast`
  - base informative toast renderer/container
  - not typically emitted directly as a payload `viewTemplateType`
  - used by the overlay to select informative layouts and summary variants

### Informative and indicator exports

- `ToastTemplateA`
  - fully covered by sample matrices
  - supports `icon` or `icons`, `message`, `subMessage`, `extraMessage`
  - also used by many system builders as the default non-interactive template

- `ToastTemplateB`
  - fully covered by sample matrices
  - used when a sender-style upper line plus main message is preferred
  - also reused by non-specialized Share Play / save-data / PS Now scenarios

- `IconOnlyToast`
  - used for pure icon status notifications
  - selected by audio/capture builders for cases like screenshot stored or
    volume mute states
  - sample exists with `VrTemplateC`
  - representative `viewData`:

```json
{
  "viewTemplateType": "IconOnlyToast",
  "viewData": {
    "icon": {
      "type": "Predefined",
      "parameters": {
        "icon": "system"
      }
    }
  }
}
```

- `DeviceConnectionIndicator`
  - concrete builder found
  - uses `viewData.devices` with `DeviceInfo` items
  - if no devices can be derived, builder falls back to `ToastTemplateA`
  - sample exists with `VrTemplateD`
  - representative `viewData`:

```json
{
  "viewTemplateType": "DeviceConnectionIndicator",
  "viewData": {
    "devices": [
      {
        "type": "DeviceInfo",
        "parameters": {
          "device": "game",
          "battery": "battery_empty"
        }
      }
    ]
  }
}
```

- `VolumeIndicator`
  - concrete builder found in audio-device notification path
  - uses `contentType: "progress"` and `progress.rate`
  - VR variants use `VrTemplateE`
  - accessibility variant uses `preMessage` plus percentage text
  - representative `viewData`:

```json
{
  "viewTemplateType": "VolumeIndicator",
  "viewData": {
    "icon": {
      "type": "Predefined",
      "parameters": {
        "icon": "sound_speaking"
      }
    },
    "contentType": "progress",
    "progress": {
      "rate": 0.25
    }
  }
}
```

- `MixSliderIndicator`
  - concrete builder found in audio-device path
  - uses left/right icons and a normalized slider `value`
  - VR variants also use `VrTemplateE`

```json
{
  "viewTemplateType": "MixSliderIndicator",
  "viewData": {
    "leftIcon": {
      "type": "Predefined",
      "parameters": {
        "icon": "sound_level_game_up_party_down"
      }
    },
    "rightIcon": {
      "type": "Predefined",
      "parameters": {
        "icon": "sound_level_party_up_game_down"
      }
    },
    "value": 0.5
  }
}
```

- `HandoverIndicator`
  - concrete builder found
  - channel `SystemFeedback`
  - `toastOverwriteType: "Always"`
  - `viewData` includes:
    - `icons`
    - `message`
    - `supportingIcons`
  - accessibility platform view concatenates message with device names

### Interactive generic exports

- `InteractiveToastTemplateA`
  - fully covered by scenario samples and matrix samples
  - compact interactive card with `actions`
  - often paired with `DeepLink`

- `InteractiveToastTemplateB`
  - fully covered by generic sample matrices
  - also reused by:
    - system message universal checkout
    - game trial countdown (`NUC559`)
    - some Share Play / PS Now paths

- `InteractiveToastSummary`
  - synthetic aggregate toast built by summary mode, not a usual app payload
  - generated with:
    - `viewTemplateType: "InteractiveToastSummary"`
    - `useCaseId: "NUCSUMMARY"`
    - `toastOverwriteType: "InQueue"`
    - `viewData.parameters.sourcePayloads`
    - callbacks like `markItemAsRead` / `markItemAsDeleted`
  - summary mode wraps multiple suppressed/deferred notifications into one

### Interactive scenario exports with concrete builders or samples

- `InteractiveToastMessages`
  - large sample catalogue exists
  - covers text, stickers, voice, image, video, music, events, invitations
  - driven mainly by `useCaseId` and media-specific `parameters`

- `InteractiveToastTrophy`
  - concrete builder found
  - channel `Trophies`
  - `bundleName: "local:Trophy"`
  - `soundEffect` depends on trophy grade
  - action goes to trophy browser via `pstc:browse?...`
  - preview-disabled and VR variants are generated
  - use cases visible in builder:
    - `NUC55`
    - `NUC273`

- `InteractiveToastPlayerSessionInvitation`
  - scenario sample exists
  - carries session-style metadata and user/title context
  - used for player-session invitation flows

- `InteractiveToastPlayerSessionRequestToJoin`
  - explicit scenario sample exists with `useCaseId: "NUC449"`
  - separate specialized template export, not only a generic A/B reuse

- `InteractiveToastLegacySessionInvitation`
  - scenario sample exists
  - uses `ConceptByTitle` placeholder and invitation/session parameters

- `InteractiveToastScreenShare`
  - concrete builder found
  - primary dedicated use case is `NUC104`
  - uses `FromUser` icon and an `ActionCardLink` action with
    `type: "ActionCardScreenShare"`
  - related non-specialized cases route to `ToastTemplateA` or `ToastTemplateB`
  - builder-visible use cases:
    - `NUC104`
    - `NUC171`
    - `NUC176`
    - `NUC177`
    - `NUC178`
    - `NUC359`
    - `NUC361`
    - `NUC378`
    - `NUC379`
    - `NUC450`
    - `NUC453`

- `InteractiveToastSharePlay`
  - concrete builder found
  - dedicated interactive template used for `NUC107`
  - many other Share Play use cases route to `ToastTemplateA` or
    `ToastTemplateB`
  - builder-visible Share Play use cases include:
    - `NUC107`
    - `NUC132`
    - `NUC133`
    - `NUC135`
    - `NUC136`
    - `NUC137`
    - `NUC138`
    - `NUC139`
    - `NUC140`
    - `NUC141`
    - `NUC142`
    - `NUC143`
    - `NUC145`
    - `NUC147`
    - `NUC148`
    - `NUC149`
    - `NUC150`
    - `NUC151`
    - `NUC152`
    - `NUC154`
    - `NUC155`
    - `NUC156`
    - `NUC158`
    - `NUC160`
    - `NUC162`
    - `NUC168`
    - `NUC170`
    - `NUC362`
    - `NUC363`
    - `NUC380`
    - `NUC381`
    - `NUC382`
    - `NUC383`
    - `NUC384`
    - `NUC385`
    - `NUC386`
    - `NUC407`
    - `NUC415`
    - `NUC436`

- `InteractiveToastFriendRequest`
  - concrete scenario samples exist for `NUC1` and `NUC2`
  - rich top-level metadata present in samples

- `InteractiveToastFriendAvailable`
  - simple sample exists
  - compact `FromUser` + `%fromUser%` pattern

- `InteractiveToastActivityChallenges`
  - large scenario sample catalogue exists
  - one of the richest placeholder/parameter families in the bundle

- `InteractiveToastVoiceChat`
  - large scenario sample catalogue exists
  - frequent `toastOverwriteType: "InQueue"`
  - party/session parameters in `viewData.parameters`

- `InteractiveToastSystemMessage`
  - dedicated sample exists
  - nearby scenarios also show that some “system message” experiences can use
    generic `InteractiveToastTemplateB` instead

- `InteractiveToastGamePreparation`
  - strongly documented elsewhere in this file
  - specialized game-ready/download/storage CTA template

- `InteractiveToastTournaments`
  - sample catalogue exists for `NUC510-NUC516`
  - uses title-centric `AssociatedTitle` plus tournament parameters

- `InteractiveToastSaveDataMessage`
  - concrete builder found
  - dedicated template for save-data sync/storage errors
  - builder-visible use cases:
    - `NUC430`
    - `NUC431`
    - `NUC433`
    - `NUC461`
  - related use cases `NUC563-NUC566` instead route to `ToastTemplateB`
  - notable fields:
    - `associatedTitle`
    - title placeholder in `subMessage`
    - warning-style `message`
    - optional `uploadData`
    - deep link to save-data sync details

- `InteractiveToastPSNowPlayer`
  - concrete builder found
  - dedicated template used for `NUC192`
  - nearby use cases `NUC190` and `NUC280` use `InteractiveToastTemplateB`
  - icon switches between `ps_now` and `ps_plus` depending on entitlement
  - actions can include:
    - `DeepLink` to `pscloudplayer:play?...`
    - `PlayerControl` with `command: "CancelGame"`

- `InteractiveToastGameToPlayer`
  - concrete interactive renderer found in bundle module `2028`
  - rendered through the generic interactive container with:
    - `template: "TemplateB"`
    - custom expanded header/body renderer
  - expanded header uses:
    - `model.viewData.icon`
    - `model.viewData.subMessage` as the upper game-title line
    - `model.associatedTitle`
  - expanded body uses:
    - optional `model.viewData.parameters.text`
    - `GameCTA` button area in horizontal compact layout
  - supports async `preFetch(model, telemetryProps)` and, when
    `viewData.parameters.activityId` is present, resolves:
    - `uamInfo` via `getUamInfoObject(...)`
    - `ctaData` via `GameIntentUtility.formatToCtaData(...)`
  - representative model shape implied by the renderer:

```json
{
  "viewTemplateType": "InteractiveToastGameToPlayer",
  "associatedTitle": {
    "npTitleId": "PPSA00000_00"
  },
  "viewData": {
    "icon": {
      "type": "TitleByTitle",
      "parameters": {
        "npTitleIds": ["PPSA00000_00"]
      }
    },
    "subMessage": {
      "body": "Game title / source line"
    },
    "parameters": {
      "activityId": "activity-id",
      "text": "Expanded body text"
    }
  }
}
```

- `InteractiveToastAllowListRequest`
  - concrete interactive renderer found in bundle module `2187`
  - rendered through the generic interactive container with:
    - `template: "TemplateB"`
    - async prefetch of title/concept metadata
  - `preFetch(model)` requires `viewData.parameters.associatedTitle` and looks
    up:
    - `objectType: "ConceptByTitle"`
    - `npTitleIds: [associatedTitle]`
  - expanded body shows:
    - loading spinner while prefetch runs
    - title/content list item on success
    - partial error state on failure
    - CTA area below the fetched title info
  - CTA behavior depends on `useCaseId`:
    - `NUC469`, `NUC473`: deep link to allowed-games settings for the
      requester (`fromUser`)
    - `NUC470`, `NUC474`: `GameCTA` for `associatedTitle`
    - `NUC471`, `NUC472`, `NUC475`, `NUC476`: plain `OK` button
  - visible parameter fields:
    - `viewData.parameters.associatedTitle`
    - `viewData.parameters.fromUser`
  - representative model shape implied by the renderer:

```json
{
  "viewTemplateType": "InteractiveToastAllowListRequest",
  "useCaseId": "NUC469",
  "viewData": {
    "parameters": {
      "associatedTitle": "PPSA00000_00",
      "fromUser": "account-or-requester-id"
    }
  }
}
```

- `InteractiveToastVoiceCommandUserFeedback`
  - concrete builder found
  - channel `ServiceFeedback`
  - `viewData.parameters` includes:
    - `useCaseId`
    - `agentIntentSessionId`
    - `voiceLanguage`
    - `label` object with `icon`, `primaryText`, `subText`
  - uses dedicated template rather than generic A/B

- `InteractiveToastSuppressionOnboarding`
  - explicit send sample found for `useCaseId: "NUC411"`
  - uses:
    - `isSummaryProhibited: true`
    - `platformViews.previewDisabled`
    - onboarding/help-style icon and text
  - clearly separate from normal runtime notifications

### Exports with limited evidence in the bundle slices searched

- `TransitionSample`
  - explicit sample exists in the template sample catalogue
  - logged form:

```json
{
  "rawData": {
    "viewTemplateType": "TransitionSample",
    "useCaseId": "NUC_TRANSITION_SAMPLE",
    "channelType": "ServiceFeedback",
    "toastOverwriteType": "No",
    "isImmediate": false,
    "priority": 1,
    "viewData": {
      "icon": {
        "type": "Predefined",
        "parameters": {
          "icon": "system"
        }
      },
      "message": {
        "body": "Transition sample for %fromUser% (%title%)"
      }
    }
  }
}
```

## Template A vs Template B

### `InteractiveToastTemplateA`

Typical shape:

```json
{
  "rawData": {
    "viewTemplateType": "InteractiveToastTemplateA",
    "useCaseId": "NUC_XXX",
    "channelType": "ServiceFeedback",
    "toastOverwriteType": "No",
    "isImmediate": true,
    "priority": 100,
    "viewData": {
      "icon": { ... },
      "message": { "body": "PrimaryContent" },
      "actions": [ ... ]
    }
  }
}
```

Characteristics:

- compact interactive card
- primary text line
- optional actions

Samples:

### `InteractiveToastTemplateB`

Typical shape:

```json
{
  "rawData": {
    "viewTemplateType": "InteractiveToastTemplateB",
    "useCaseId": "IDC",
    "channelType": "Downloads",
    "toastOverwriteType": "No",
    "isImmediate": true,
    "priority": 100,
    "viewData": {
      "icon": { ... },
      "subMessage": { "body": "Sender" },
      "message": { "body": "PrimaryContent" },
      "extraMessage": { "body": "TertiaryContent" },
      "actions": [ ... ]
    }
  }
}
```

Characteristics:

- `subMessage` is the upper/secondary line
- `message` is the main line
- optional `extraMessage`
- actions supported

Samples:

## `InteractiveToastGamePreparation`

This is a specialized interactive template for game-ready, DLC-ready,
download-failed, storage-shortage, and related game-preparation scenarios.

Builder and helper code:

### Typical shape

```json
{
  "rawData": {
    "viewTemplateType": "InteractiveToastGamePreparation",
    "channelType": "Downloads",
    "useCaseId": "NUC238",
    "toastOverwriteType": "No",
    "isImmediate": true,
    "priority": 1,
    "bundleName": "PPSA00000",
    "viewData": {
      "icon": {
        "type": "TitleByTitle",
        "parameters": {
          "npTitleIds": ["PPSA00000_00"]
        }
      },
      "message": {
        "body": "Installed."
      },
      "subMessage": {
        "body": "%titleByTitle%",
        "placeHolders": {
          "titleByTitle": {
            "objectType": "TitleByTitle",
            "npTitleIds": ["PPSA00000_00"],
            "defaultValue": "Unknown"
          }
        }
      },
      "parameters": {
        "expandedType": "GameCTA",
        "titleId": "PPSA00000"
      }
    }
  }
}
```

### Observed `expandedType` values

- `GameCTA`
- `StorageShortage`
- `ViewGame`


### `GameCTA` constraints

The bundle explicitly logs that GameCTA:

- does not support `titleId` input on `CommonDialog`
- is supported only on `ShellUI` or `CommonDialog`


### Use-case mapping observed in bundle

- `NUC238` -> `gameReadyToPlay`
- `NUC241` -> `dlcReadyToPlay`
- `NUC253` -> `insufficientSystemStorage`
- `NUC255` -> `insufficientExtendedStorage`
- `NUC257` -> `downloadFailed`
- `NUC478` -> `insufficientM2Storage`
- `NUC547` -> `addonsReadyToPlay`


### Extra notes

`InteractiveToastGamePreparation` can also carry:

- `parameters.actionProps`
- storage-related parameters
- title/content IDs
- `uploadData` in some builders


## Informative and Indicator-Style Templates

Non-interactive families include:

- `ToastTemplateA`
- `ToastTemplateB`
- `IconOnlyToast`
- `DeviceConnectionIndicator`
- `VolumeIndicator`

Observed payload differences:

- they often use `viewData.message` and `viewData.subMessage` without actions
- some indicator templates use:
  - `viewData.devices`
  - `viewData.icons`
  - `viewData.text`
  - `viewData.minimal`

Examples:
- `DeviceConnectionIndicator` with `devices`
- `IconOnlyToast` with icon only
- compact indicators using `icons` + `text`


## Metadata, Visibility, and Suppression

### Ordering

Notifications are sorted by `createdDateTime`.


### Replacement

Replacement behavior depends on:

- `toastOverwriteType`
- `bundleName`
- `useCaseId`
- `id`


### In-context suppression

Notifications can be suppressed based on:

- `useCaseId`
- `bundleName`
- or both


### No-login-user behavior

The bundle suppresses some notifications when:

- there are no login users
- the payload depends on title/user metadata
- or a `localNotificationId` path is involved


### `offConsoleToastType`

The bundle contains off-console suppression handling with:

- `Always`
- `OnDemand`

`OnDemand` behavior depends on whether `notificationId` or
`localNotificationId` is already known.


## Event Types Observed in Local Dispatch

The local overlay dispatch path handles:

- `send`
- `sendById`
- `debug`
- `idu`
- `vrIndicator`

For `send`, it parses `payload` as JSON.


## Practical Guidance for Local Payloads

For locally generated notifications in a payload like ShadowMount, the safest
subset inferred from the bundle is:

- use `rawData`
- always provide:
  - `viewTemplateType`
  - `useCaseId`
  - `channelType`
  - `viewData`
- prefer `isImmediate=true` if immediate popup is desired
- use `toastOverwriteType="No"` unless replacement is intentional
- use `bundleName` only when grouping/replacement is wanted
- keep `InteractiveToastTemplateB` payloads simple unless you really need a
  specialized system template
- if using `InteractiveToastGamePreparation`, expect much stricter assumptions
- if using title/user placeholders, provide valid title/user context
- if using `localNotificationId`, keep it non-empty

## Minimal Example: Generic Interactive B

```json
{
  "rawData": {
    "viewTemplateType": "InteractiveToastTemplateB",
    "channelType": "Downloads",
    "useCaseId": "IDC",
    "toastOverwriteType": "No",
    "isImmediate": true,
    "priority": 100,
    "viewData": {
      "icon": {
        "type": "Url",
        "parameters": {
          "url": "/user/data/shadowmount/smp_icon.png"
        }
      },
      "message": {
        "body": "PrimaryContent"
      },
      "subMessage": {
        "body": "SecondaryContent"
      }
    }
  },
  "createdDateTime": "2026-03-09T12:00:00.000Z",
  "updatedDateTime": "2026-03-09T12:00:00.000Z",
  "expirationDateTime": "2026-03-09T13:00:00.000Z",
  "localNotificationId": "123456789"
}
```

## Minimal Example: Game Preparation

```json
{
  "rawData": {
    "viewTemplateType": "InteractiveToastGamePreparation",
    "bundleName": "PPSA00000",
    "channelType": "Downloads",
    "useCaseId": "NUC238",
    "toastOverwriteType": "No",
    "isImmediate": true,
    "priority": 1,
    "viewData": {
      "icon": {
        "type": "TitleByTitle",
        "parameters": {
          "npTitleIds": ["PPSA00000_00"]
        }
      },
      "message": {
        "body": "Installed."
      },
      "parameters": {
        "expandedType": "GameCTA",
        "titleId": "PPSA00000"
      },
      "subMessage": {
        "body": "%titleByTitle%",
        "placeHolders": {
          "titleByTitle": {
            "defaultValue": "Unknown",
            "npTitleIds": ["PPSA00000_00"],
            "objectType": "TitleByTitle"
          }
        }
      }
    }
  },
  "createdDateTime": "2026-03-09T12:00:00.000Z",
  "updatedDateTime": "2026-03-09T12:00:00.000Z",
  "expirationDateTime": "2026-03-09T13:00:00.000Z",
  "localNotificationId": "987654321"
}
```

## Extracted Scenario Sample Catalogue

The bundle also contains a large catalogue of concrete sample notifications in
the range around `332904-335303`. Those examples are useful because they show
real `useCaseId`, `channelType`, placeholder, and `parameters` combinations,
not only generic template skeletons.

### `InteractiveToastActivityChallenges`

Found examples:

- `NUC264` points/time: reclaim the lead
- `NUC265` points/time: challenge completed
- `NUC269` points/time: fell out of top 100
- `NUC271` points/time: final global placement
- `NUC272` points/time: challenge ended
- `NUC345` points/time: direct challenge from another user
- `NUC463` points/time: personal best

Observed shape:

```json
{
  "viewTemplateType": "InteractiveToastActivityChallenges",
  "associatedTitle": {
    "npTitleId": "NPXS45020_00"
  },
  "fromUser": {
    "accountId": "3184494961250237665"
  },
  "useCaseId": "NUC264",
  "channelType": "ActivityChallenges",
  "viewData": {
    "icon": {
      "type": "AssociatedTitle"
    },
    "subMessage": {
      "body": "%fromUser%",
      "placeHolders": {
        "fromUser": {
          "objectType": "User",
          "accountId": "3184494961250237665",
          "defaultValue": "Default User"
        }
      }
    },
    "message": {
      "body": "Reclaim the lead in %challengeName%?",
      "placeHolders": {
        "challengeName": {
          "objectType": "String",
          "defaultValue": "Defeat Raid Boss"
        }
      }
    },
    "parameters": {
      "npCommunicationId": "NPWR17252_00",
      "challengeName": "Defeat Raid Boss",
      "runId": "faf0d1d0-5d20-435b-bb8b-b620c5a33714",
      "challengeId": "challenge05"
    }
  }
}
```

Notes:

- uses `AssociatedTitle` icon heavily
- mixes `User`, `String`, `Title`, and `Number` placeholders
- `parameters` carry challenge identity and scoring metadata

### `InteractiveToastFriendAvailable`

Observed sample:

```json
{
  "fromUser": {
    "accountId": "1000187643835253666"
  },
  "viewData": {
    "icon": {
      "type": "FromUser"
    },
    "message": {
      "body": "Your friend just came online."
    },
    "subMessage": {
      "body": "%fromUser%",
      "placeHolders": {
        "fromUser": {
          "objectType": "User",
          "accountId": "1000187643835253666",
          "defaultValue": "default user name"
        }
      }
    }
  },
  "viewTemplateType": "InteractiveToastFriendAvailable"
}
```

### `InteractiveToastFriendRequest`

Found examples:

- `NUC1` standard friend request
- `NUC1` close friend request variant
- `NUC2` real-name / close-friends wording

Observed shape:

```json
{
  "viewTemplateType": "InteractiveToastFriendRequest",
  "channelType": "FriendRequest",
  "createdDateTime": "2019-06-14T19:38:13.155Z",
  "expirationDateTime": "2100-09-12T19:38:13.155Z",
  "fromUser": {
    "accountId": "7066465281289280957"
  },
  "notificationId": "399498519847690",
  "priority": 2,
  "state": "New",
  "updatedDateTime": "2019-06-14T19:38:13.197Z",
  "useCaseId": "NUC1",
  "viewData": {
    "icon": {
      "type": "FromUser"
    },
    "message": {
      "body": "%fromUser%",
      "placeHolders": {
        "fromUser": {
          "objectType": "User",
          "accountId": "7066465281289280957",
          "defaultValue": "SUGITV"
        },
        "usersCount": {
          "objectType": "Number",
          "defaultValue": 1
        }
      }
    },
    "subMessage": {
      "body": "Wants to become friends",
      "placeHolders": {}
    },
    "parameters": {
      "fromUser": "7066465281289280957",
      "fromUsers": ["7066465281289280957"],
      "totalCount": 1,
      "usersCount": 1
    }
  }
}
```

Notes:

- unlike minimal local payloads, system samples often include top-level
  `createdDateTime`, `updatedDateTime`, `expirationDateTime`, `state`,
  `notificationId`
- message text is often driven by `%fromUser%`, with wording moved to
  `subMessage`

### `InteractiveToastLegacySessionInvitation`

Observed sample characteristics:

- `notificationGroup: "np:session:invite"`
- `ConceptByTitle` placeholder via `associatedTitles`
- `parameters.invitationId`
- title and user metadata both present

Representative excerpt:

```json
{
  "viewTemplateType": "InteractiveToastLegacySessionInvitation",
  "associatedTitle": {
    "npTitleId": "NPXS29104_00"
  },
  "fromUser": {
    "accountId": "5214476082371262855"
  },
  "viewData": {
    "icon": {
      "type": "FromUser"
    },
    "message": {
      "body": "Invited you to play %associatedTitles%.",
      "placeHolders": {
        "associatedTitles": {
          "objectType": "ConceptByTitle",
          "npTitleIds": ["NPXS29104_00"],
          "defaultValue": "NPXS29104"
        }
      }
    },
    "subMessage": {
      "body": "%fromUser%",
      "placeHolders": {
        "fromUser": {
          "objectType": "User",
          "accountId": "5214476082371262855",
          "defaultValue": "qd5ccba95d5-US"
        }
      }
    }
  }
}
```

### `InteractiveToastMessages`

Found examples:

- `NUC63`, `NUC181` add group
- `NUC64`, `NUC182` join group
- `NUC65`, `NUC183` join group with 3/4 players
- `NUC69`, `NUC174` message reply
- `NUC108`, `NUC179` sticker
- `NUC109` voice
- `NUC110` image
- `NUC111` music
- `NUC112` video
- `NUC116` invitation / user scheduled event
- `NUC117` official event
- `NUC351-356` mixed media + text

Representative message-group example:

```json
{
  "viewTemplateType": "InteractiveToastMessages",
  "useCaseId": "NUC63",
  "channelType": "Messages",
  "viewData": {
    "icon": {
      "type": "FromUser"
    },
    "subMessage": {
      "body": "%fromUser%",
      "placeHolders": {
        "fromUser": {
          "objectType": "User",
          "accountId": "1000187643835253666",
          "defaultValue": "default user name"
        }
      }
    },
    "message": {
      "body": "Added you to a group."
    }
  }
}
```

Family notes:

- this family is the densest consumer of mixed media variants
- many examples keep the same template while only changing `useCaseId`,
  message wording, and `parameters`

### `InteractiveToastPlayerSessionInvitation`

Observed family:

- player-session invitation sample
- player-session request-to-join sample (`NUC449`)

Representative characteristics:

- session-specific `parameters`
- player-session oriented template name rather than generic A/B
- usually combines user context with session metadata

### `InteractiveToastSystemMessage`

Observed sample:

- `"System Message Multiple Links"` uses
  `viewTemplateType: "InteractiveToastSystemMessage"`
- nearby `"System Message Universal Checkout"` instead uses
  `InteractiveToastTemplateB`

Implication:

- system-message scenarios are not tied to a single template
- same feature area can route through either a specialized template or generic
  `InteractiveToastTemplateB`

### `InteractiveToastTemplateA` scenario samples

Found direct scenario uses:

- `NUC131` left from group
- two `associatedTitle + fromUser` samples with:
  - `bundleName`
  - `toastOverwriteType: "Always"`
  - `isImmediate: false`
  - `DeepLink` action to notification list

Representative excerpt:

```json
{
  "bundleName": "testGroup1",
  "toastOverwriteType": "Always",
  "priority": 1,
  "channelType": "testGroup1",
  "isImmediate": false,
  "useCaseId": "NUCTESTG1",
  "associatedTitle": {
    "npTitleId": "CUSA00001_00"
  },
  "fromUser": {
    "accountId": "1832258314344407885"
  },
  "viewTemplateType": "InteractiveToastTemplateA",
  "viewData": {
    "icon": {
      "type": "AssociatedTitle"
    },
    "message": {
      "body": "<<< %fromUser% >>>"
    },
    "subMessage": {
      "body": "((( %associatedTitle% )))"
    },
    "actions": [
      {
        "actionName": "Notification List",
        "actionType": "DeepLink",
        "defaultFocus": true,
        "parameters": {
          "actionUrl": "psnotificationlist:play"
        }
      }
    ]
  }
}
```

### `UnmSample`, `MessageReplySample`, `CTASample`, `LargeSizeExpandSample`

The bundle also includes explicit sample-only templates:

- `UnmSample`
- `MessageReplySample`
- `CTASample`
- `LargeSizeExpandSample`

Representative shapes:

```json
{
  "viewTemplateType": "UnmSample",
  "useCaseId": "NUC_UNM_SAMPLE",
  "channelType": "ServiceFeedback",
  "toastOverwriteType": "No",
  "isImmediate": false,
  "priority": 1,
  "viewData": {
    "icon": {
      "type": "FromUser"
    },
    "message": {
      "body": "UNM sample for %fromUser%"
    },
    "subMessage": {
      "body": "Notification for UNM sample"
    },
    "parameters": {
      "message1": "message from feature server 1",
      "message2": "message from feature server 2"
    }
  }
}
```

```json
{
  "viewTemplateType": "MessageReplySample",
  "useCaseId": "NUCtesttest",
  "viewData": {
    "icon": {
      "type": "Predefined",
      "parameters": {
        "icon": "community"
      }
    },
    "message": {
      "body": "Someone"
    },
    "subMessage": {
      "body": "send to me a message"
    }
  }
}
```

```json
{
  "viewTemplateType": "CTASample",
  "useCaseId": "NUC_CTA_SAMPLE",
  "channelType": "ServiceFeedback",
  "toastOverwriteType": "No",
  "isImmediate": false,
  "priority": 1,
  "viewData": {
    "icon": {
      "type": "Predefined",
      "parameters": {
        "icon": "system"
      }
    },
    "message": {
      "body": "PrimaryContent"
    },
    "subMessage": {
      "body": "SecondaryContent"
    },
    "parameters": {
      "type": "3"
    }
  }
}
```

### `InteractiveToastVoiceChat`

Found examples:

- `NUC66` party started with you
- `NUC185` private party start / invitation
- `NUC527` open party invite
- `NUC528` open party invite with screen share
- `NUC532` request to join

Representative shape:

```json
{
  "viewTemplateType": "InteractiveToastVoiceChat",
  "useCaseId": "NUC527",
  "channelType": "Party",
  "toastOverwriteType": "InQueue",
  "isImmediate": false,
  "priority": 1,
  "fromUser": {
    "accountId": "64271530114044507"
  },
  "viewData": {
    "icon": {
      "type": "FromUser"
    },
    "message": {
      "body": "Invited you to a party."
    },
    "subMessage": {
      "body": "%fromUser%"
    },
    "parameters": {
      "sessionId": "sessionId",
      "fromUser": "64271530114044507"
    }
  }
}
```

Notes:

- often uses `toastOverwriteType: "InQueue"`
- `parameters` carry `groupId` or `sessionId`

### `InteractiveToastTournaments`

Found examples:

- `NUC510` tournament started
- `NUC511` join match
- `NUC512` round win
- `NUC513` next round ready
- `NUC514` tournament lost
- `NUC515` tournament won
- `NUC516` tournament cancelled

Representative shape:

```json
{
  "viewTemplateType": "InteractiveToastTournaments",
  "associatedTitle": {
    "npTitleId": "NPXS45020_00"
  },
  "useCaseId": "NUC510",
  "channelType": "Tournaments",
  "viewData": {
    "icon": {
      "type": "AssociatedTitle"
    },
    "subMessage": {
      "body": "%associatedTitles%"
    },
    "message": {
      "body": "Your tournament started. You have 5 minutes to join your match."
    },
    "parameters": {
      "remainingDuration": 5,
      "pairingId": "36ab0940-2247-4b29-9502-a6819a4b09f7",
      "occurrenceId": "7b92ae52-5ee5-43df-8d34-d9c282c13152"
    }
  }
}
```

## Extracted Template Matrix Examples

In addition to scenario-specific samples, the bundle contains large template
stress-test matrices around `338943-341001`.

These are especially useful because they reveal truncation, line-count, icon,
action, and `logged/rawData` behavior.

### Informative `ToastTemplateA` matrix

Observed variants include:

- `message` only
- `message + subMessage`
- `message + subMessage + extraMessage`
- very long `message` and `subMessage` strings to test ellipsis
- `icons` array with two predefined icons

Representative extracted examples:

```json
{
  "viewTemplateType": "ToastTemplateA",
  "viewData": {
    "icon": {
      "type": "Predefined",
      "parameters": {
        "icon": "system"
      }
    },
    "message": {
      "body": "PrimaryContent"
    }
  }
}
```

```json
{
  "viewTemplateType": "ToastTemplateA",
  "viewData": {
    "icons": [
      {
        "type": "Predefined",
        "parameters": {
          "icon": "family"
        }
      },
      {
        "type": "Predefined",
        "parameters": {
          "icon": "ps_user"
        }
      }
    ],
    "message": {
      "body": "PrimaryContent"
    },
    "subMessage": {
      "body": "SecondaryContent"
    }
  }
}
```

### Informative `ToastTemplateB` matrix

Observed variants emphasize:

- `subMessage` as sender line
- `message` as primary content
- optional `extraMessage`
- long sender/main strings for ellipsis behavior

Representative extracted example:

```json
{
  "viewTemplateType": "ToastTemplateB",
  "viewData": {
    "icon": {
      "type": "Predefined",
      "parameters": {
        "icon": "system"
      }
    },
    "subMessage": {
      "body": "Sender"
    },
    "message": {
      "body": "PrimaryContent"
    },
    "extraMessage": {
      "body": "TertiaryContent"
    }
  }
}
```

### Interactive `TemplateA` / `TemplateB` matrix

The interactive matrices mirror informative samples but add `actions`, usually
through a shared action array.

Representative extracted example:

```json
{
  "viewTemplateType": "InteractiveToastTemplateA",
  "viewData": {
    "icon": {
      "type": "Predefined",
      "parameters": {
        "icon": "system"
      }
    },
    "message": {
      "body": "PrimaryContent"
    },
    "actions": [
      {
        "actionType": "DeepLink"
      },
      {
        "actionType": "ActionCardLink"
      }
    ]
  }
}
```

```json
{
  "viewTemplateType": "InteractiveToastTemplateB",
  "viewData": {
    "icon": {
      "type": "Predefined",
      "parameters": {
        "icon": "system"
      }
    },
    "subMessage": {
      "body": "Sender"
    },
    "message": {
      "body": "PrimaryContent"
    },
    "extraMessage": {
      "body": "TertiaryContent"
    },
    "actions": [
      {
        "actionType": "DeepLink"
      }
    ]
  }
}
```

### Logged vs non-logged payload form

The sample matrix also shows two delivery forms:

- non-logged/direct:

```json
{
  "viewTemplateType": "InteractiveToastTemplateA",
  "useCaseId": "NUC_xxx",
  "channelType": "ServiceFeedback",
  "viewData": { ... }
}
```

- logged/local-wrapper:

```json
{
  "rawData": {
    "viewTemplateType": "InteractiveToastTemplateA",
    "useCaseId": "NUC_xxx",
    "channelType": "ServiceFeedback",
    "viewData": { ... }
  }
}
```

That matches the dispatch behavior elsewhere in the bundle:

- logged local send paths often wrap the payload in `rawData`
- direct sample sends can still use the bare template object
