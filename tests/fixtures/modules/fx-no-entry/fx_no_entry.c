/* fx-no-entry: a library that exports no praenora_module_get_api. Its only function has hidden
 * visibility, so nothing is exported at all. C99. */
int fx_no_entry_answer(void);

int fx_no_entry_answer(void) { return 42; }
