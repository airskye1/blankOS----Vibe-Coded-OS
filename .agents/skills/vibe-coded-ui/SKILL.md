---
name: vibe-coded-ui
description: UI pattern rules and vibe guidelines for designing the OS interface.
---

# UI patterns

Design all UI patterns as part of one system. A modal should feel nearer than a card, and a dropdown should feel nearer than the page surface, with those depth relationships staying consistent across components. Keep palette, spacing, radius, type, and elevation mapped to shared tokens so every component feels related.

### Modals

Use modals sparingly. Modal dialogs are best for important warnings, critical information required to continue, or actions that truly need the user’s full attention, and they are a poor fit for nonessential information or complex decisions that need outside context.

#### Good modal rules
- Use modals for short, focused tasks only, like confirm delete, sign in, quick create, or short settings.
- Let the user trigger the modal whenever possible; do not surprise users with random popups.
- Keep the headline clear and specific, and use explicit action labels instead of vague “Yes/No” buttons.
- Provide obvious dismissal paths: close button, cancel button, Escape key, and usually click-outside close.
- Trap focus inside the modal and return focus to the trigger when it closes.
- Prevent background scroll while the modal is open, and restore it correctly after close.
- On mobile, prefer full-screen or bottom-sheet style dialogs instead of tiny centered boxes.

#### Bad modal rules
- Do not put long multi-step workflows in a modal; if it needs scrolling and multiple columns, it deserves its own page.
- Do not use modals for ads, walkthrough spam, noncritical tips, or content the user may want to compare against the background page.
- Do not make the modal visually heavier than necessary.
- Do not hide the close action.
- Do not let the background change underneath the modal.

#### Modal layout advice
A good modal usually has:
- clear title
- short description
- one primary action
- one secondary action
- optional close button
- focused content area
- enough padding to breathe

Use a dimmed backdrop, but not pitch black; the user should still perceive the page behind it while clearly understanding focus has moved to the dialog.

#### Modal animation advice
Animate modals quickly and clearly. Short fade/slide/scale transitions are easier on the eyes than long dramatic entrances, and the modal should feel like it comes forward while the backdrop recedes.

Good modal animation:
- backdrop fades in quickly
- modal fades + slightly translates or scales in
- blur can be used lightly on the backdrop or entry state
- modal feels nearer than cards and dropdowns
- animation duration is short and consistent

Bad modal animation:
- bouncing modal
- giant zoom
- heavy blur bloom
- delayed close
- over-complicated choreography

### Sheets and drawers

Use sheets or drawers when the UI needs more room than a tiny modal but still should stay in context. On small screens, a bottom sheet often feels more natural than a centered dialog because it uses the available space better and preserves spatial continuity.

Use sheets for:
- filters
- mobile menus
- lightweight settings
- quick details
- action panels

Do not use sheets for:
- giant forms
- complex dashboards
- multi-step flows that deserve their own page

### Dropdowns and popovers

Dropdowns and popovers should feel lighter than modals and heavier than inline UI. Keep them short, anchored to a trigger, and easy to dismiss. If the content becomes scrollable, multi-column, or highly interactive, switch to a modal, sheet, or dedicated page instead.

Good rules:
- keep content short
- anchor clearly to the trigger
- use strong alignment and spacing
- close on outside click and Escape
- maintain keyboard support

Bad rules:
- giant mega-dropdowns for complex tasks
- hiding critical features in unstable popovers
- using popovers as mini pages

### Topbars / app bars

Top app bars exist to display navigation, page identity, and screen-specific actions at the top of a screen. A top bar should breathe, have enough height, clearly communicate the page, and avoid feeling cramped.

#### Good topbar rules
- Use the center or left area for page identity, usually title or logo.
- Group actions logically on the right.
- Give the bar enough height so icons and text do not look squished.
- Keep the action count small.
- Let the topbar reflect the current page, not the whole product at once.

#### Bad topbar rules
- Too many actions.
- Tiny tap targets.
- Center area filled with decorative junk.
- Topbar trying to be navbar, breadcrumb, search area, notification center, and settings panel all at once.

#### Topbar variants
Use different topbar types depending on the screen:
- Marketing topbar: logo, nav links, CTA.
- App topbar: page title, back button if needed, contextual actions.
- Dashboard topbar: page title, filters/search, utility actions.
- Mobile topbar: simpler title + one or two key actions.
- Editor topbar: file name, save state, share/export actions.

#### Topbar motion
Topbars should move carefully:
- subtle shrink on scroll
- subtle background solidification on scroll
- no dramatic flying movement
- keep controls stable and predictable

### Sidebars

Use sidebars for information-dense apps, dashboards, editors, and admin tools. Sidebars should provide stable navigation and should not fight the topbar.

Good sidebar rules:
- stable width
- clear active state
- grouped sections
- readable labels
- collapsed mode only if needed

Bad sidebar rules:
- too many nested levels
- decorative icons everywhere
- unstable width changes
- collapsing by surprise

### Tabs

Tabs are for switching between closely related views, not unrelated destinations. Keep labels short and clear.

Good tab rules:
- limited number of tabs
- visible active state
- preserve user context
- animate the active indicator subtly

Bad tab rules:
- using tabs as full site navigation
- stuffing too much content behind tabs
- hiding important content in overflowed tab bars

### Forms

Forms should feel simple, readable, and calm.

Good form rules:
- label above input
- clear validation
- enough vertical spacing
- one primary action
- related fields grouped
- helper text only where it matters

Bad form rules:
- too many columns on mobile
- placeholder as the only label
- giant bordered input soup
- weak error states
- too many competing actions

### Tables and dense data UI

Use tables only when comparison matters. On smaller screens, collapse or transform the layout instead of crushing it.

Good rules:
- clear headers
- consistent alignment
- sticky header if needed
- row hover subtle, not flashy
- controlled density

Bad rules:
- tiny text
- too many borders
- too many row actions
- fake card-ification when a table is actually needed

### Empty states

Empty states should be useful, not cheesy.

Good empty state:
- short explanation
- one clear next action
- optional restrained illustration
- matches product tone

Bad empty state:
- emoji emoji emoji spam
- giant illustration with no meaning
- generic “Nothing here yet” without a path forward

### Toasts and notifications

Toasts should confirm lightweight events, not communicate critical decisions. Critical or blocking actions should use a modal or inline state instead.

Good toast rules:
- short text
- disappears automatically when safe
- one optional action
- does not cover key controls

Bad toast rules:
- long paragraphs
- stacking too many at once
- using toast for destructive confirmation

### Accordions

Use accordions for optional detail, FAQs, and collapsible secondary content. Do not use them to hide essential navigation or entire workflows.

Good accordion rules:
- clear labels
- obvious open/closed state
- small number of items
- smooth, quick motion

Bad accordion rules:
- accordion inside modal inside drawer
- hiding critical steps
- giant text dumps

### Search and command bars

Search bars should be visible where search is primary, and command bars should feel fast and keyboard-friendly. If the product is dense, a command bar can be more useful than stuffing actions into a topbar.

Good rules:
- clear placeholder
- visible focus state
- recent items or suggestions when relevant
- keyboard shortcut hint if appropriate

Bad rules:
- making search decorative
- giant AI-style glowing input
- stuffing every feature into search suggestions

### Menus

Menus should be short, grouped, and obvious.

Good menu rules:
- group related actions
- put destructive items last
- support keyboard navigation
- keep icon use secondary

Bad menu rules:
- menus that become settings panels
- unclear hover tunnels
- dozens of options with no grouping

### UI depth hierarchy

Keep these relationships consistent:
- page surface
- card
- dropdown/popover
- modal/sheet
- full overlay

A modal should feel closer than a dropdown, and a dropdown should feel closer than a card, with stable spacing/elevation/radius rules across the whole product.

### Final UI advice

For every component, ask:
- Is this the right component type?
- Is it too complex for its container?
- Does it have a clear exit path?
- Does it preserve context?
- Is the spacing calm and intentional?
- Is the animation helping?
- Does it still feel like part of the same system?
