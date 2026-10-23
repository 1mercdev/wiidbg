# ADR 0004: GPL-3.0-only

Status: accepted (project owner's choice); the "or-later" clause is to be confirmed

## Decision

The project is licensed under the GNU GPL version 3. Source files carry
`SPDX-License-Identifier: GPL-3.0-only`.

## Notes

- `-only` vs `-or-later`: "only" fixes us to version 3 exactly; "or-later" lets
  recipients also use future FSF versions. The FSF recommends "or-later". It is a
  one-line change per file (and in the README) if the owner prefers "-only";
  decide **before the first public release**, since relicensing later needs
  every contributor's consent.
- devkitPro's libogc is under permissive terms, which can be combined with GPL
  code. Check the licence of any library you add later.
- Under the GPL, anyone who distributes a modified build must offer the source
  under the same licence. Binary releases should therefore link to the source.
- This is the author's understanding, not legal advice.
