# Live Export: edits to Exalted Items' own rows - DONE (Live Export 0.2.0, 2026-11-23)

Asked for by the Exalted Items session (Claude Help/exalted mod/), agreed with it point by point
the same day (its note: exalted mod/Live-Suite-Export-Compatibility.md, section "Agreed with the
Live Export session"). User: "work together to make this as smooth as possible".

Done in Live Export 0.2.0:
- Key rules exactly as Exalted Items 1.2.0's IndexModRows: "[xlt] " start (case-sensitive, one
  space); K(cell) = trimmed, one pair of quotes off, lower case; output contains "exc" -> upgrade,
  key K(input 1); else "eli" -> back to elite, else restore, key K(input 1) up to the first comma.
  (`IsExalted`, `ExaltedKey` in export_core.cpp.)
- D2RMM export: a changed [xlt] row is written whole (no removal); a removed one is written with
  enabled 0; the report says "N Exalted Items recipes changed, N removed (written with enabled 0);
  the exalted codes in them are this install's".
- Plain .mpq export (option a, both sessions recommended it): only the [xlt] rows the user changed
  or removed are kept (`ApplyCubePlain`); the untouched ones are left to Exalted Items.
- D2RMM and plain exports leave out edits to Exalted Items' own rows with a report line
  (`LeaveOutExaltedRows`): stats xlt_* other than xlt_unused_*, properties xlt-*. With Exalted
  Items 1.3.0's `stat-ids.txt` (`<loader>\exalted-items\<mod>\stat-ids.txt`, read by
  `ReadExaltedIds`) also its marker= and roll.<k>= numbers; without it (older Exalted Items) made
  stats past 486. Made stats are carried in every export (agreed with Live Properties 0.8.0,
  2026-11-24): the sure sign is the plugin's made name (stat-edits.txt col 17); else `was` empty,
  live_free_ / xlt_unused_ / live_unused_, or a made.<name> of the map (`MarkMadeStats`).
  Before, one such stat edit stopped the whole D2RMM mod.
- The export with Exalted's copies: the report now says to keep Exalted Items ON (1.1.0+).
- Test: test/check_exalted.sh (part of run_tests.sh when Exalted's gen_test is built): Exalted's
  generator on both exports keeps the 3 changed recipes, the removed one with enabled 0, and makes
  the other 340.
