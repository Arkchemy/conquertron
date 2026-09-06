# Legal notes

Written 2026-09-06. Not legal advice, and not a claim of compliance with any
particular law. It records what the project actually does with data and content
so a real assessment can start from facts.

## What this project is

Arkchemy is a non-commercial fan preservation project. It is **not affiliated
with, endorsed by, or sponsored by Activision Publishing, Inc., Toys for Bob,
or any rights holder in the Skylanders series.** "Skylanders", "Spyro" and the
game titles and logos are trademarks of their respective owners and are used
here to identify the games being discussed, which is nominative use.

## Personal data

The site publishes information about identifiable people, which is personal
data under the UK/EU GDPR and comparable laws.

| what | where from | why |
|---|---|---|
| Discord display name, user id, avatar | Discord Bot API | crediting contributors, showing progress posts and the beta-tester list |
| GitHub username, avatar | GitHub public API | crediting contributors |
| message text and timestamps | Discord channels the project runs | the public FAQ and progress feed |

Details that matter:

* **No cookies, no analytics, no tracking.** The site sets no cookies and uses
  no browser storage. There is nothing to consent to under the ePrivacy
  Directive / PECR, which is why there is no cookie banner.
* **No visitor data is collected.** Requests are served by Vercel, which keeps
  its own operational logs as a processor; the project stores nothing itself.
* **Third-party images are proxied**, so a visitor's browser never contacts
  Discord or GitHub directly and those services do not see visitors' IPs.
* **Only public information is shown.** Everything comes from a public GitHub
  profile or a message posted in the project's own Discord.
* **Lawful basis** is legitimate interest (crediting people who worked on a
  public project, and publishing the project's own announcements). Anyone who
  would rather not appear can say so and will be removed — see Contact.
* **Rights.** Access, rectification, erasure, objection and restriction can all
  be exercised by contacting the project. Removal is the practical remedy for
  everything here, since none of it is required to be retained.
* **Retention.** Nothing is stored. Every name, avatar and message is fetched
  live from Discord or GitHub when the page loads, cached briefly at the CDN,
  and disappears when it is removed at the source. Contributor credits live in
  CONTRIBUTORS.csv in this repository.
* **Children.** The project is not directed at children and asks for no
  personal data from anyone.

## Content that is not ours

**This is the project's largest legal exposure and it is not solved by a
policy document.**

These repositories contain material derived from commercially released games
and from fan wikis:

* voice clips extracted from a retail disc
* character renders and roster portraits from the Skylanders wiki
* game logos and box art

That material is copyrighted by its owners. It is included for documentation
and preservation, non-commercially, and the project makes no ownership claim.
Whether any given use is defensible varies by jurisdiction, and "fan project"
is not itself a legal defence anywhere.

Note the tension deliberately: **LICENSE clause 6 states that no project under
it hosts or distributes game content.** The repositories currently do. Either
the clause or the content should change; this file exists so that is a
decision rather than an oversight.

The recompiler itself contains no game code or assets. It transforms a copy the
user already owns, on the user's own machine, and no build output is
distributed.

## Takedown

If you hold rights in anything here and want it removed, contact the project
and it will be taken down promptly. No counter-notice process, no argument, no
requirement to prove ownership beyond a plausible claim. This is a hobby
project and nothing in it is worth a dispute.

## Accessibility

The site targets WCAG 2.2 AA. What is in place: one `h1` and a heading order
with no skipped levels, a skip-to-content link, visible focus rings, text
alternatives on every image, colour contrast checked per element theme (worst
case 6.4:1 against white), motion that fully collapses under
`prefers-reduced-motion`, and a layout that reflows to 320px without horizontal
scrolling. Not yet verified: screen-reader testing with a real assistive
technology, and keyboard traversal of the marquee.

## Warranty

Provided as-is, with no warranty. Nothing here is a guarantee that running any
of it is lawful where you live; that is your responsibility.

## Contact

Open an issue at <https://github.com/Arkchemy> or reach the project through its
Discord.
