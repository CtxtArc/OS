### 1. The "Big Five" (Structural Requirements)

* [ ] **Stack Frames (`EBP`):** Currently, your code uses global physical addresses (`3000`, `3004`). You need to push `EBP` on `CALL` and set `EBP = ESP`. This allows functions to have "Local Variables" that exist only while the function is running.
* [ ] **Scoped Local Variables:** The ability to declare a variable inside a `LABEL` or `CALL` that automatically cleans itself up when the function returns (`ADD ESP, size`).
* [ ] **Struct Offsets:** A `STRUCT` directive that generates a lookup table.
* *Instead of:* `READBYTE ptr index var`
* *You want:* `SET var ptr[index]`


* [ ] **Pointer Math/Dereferencing:** A primitive to get the address of a variable (`SET ptr &my_var`) and the ability to dereference it (`SET val *ptr`).
* [ ] **Formal Function ABI:** Define a rule (e.g., "Arguments go in EAX, EBX, ECX, EDX") so you don't have to worry about which registers your `CALL`s are clobbering.

### 2. The "Usability" Layer (Compiler Features)

* [ ] **Expression Parsing (Shunting-Yard):** This is the single biggest "win."
* *Current:* `SET val 5`, `ADD val 10`, `MUL val 2`
* *Future:* `SET val (5 + 10) * 2`
* This is the difference between "Assembler" and "Compiler."


* [ ] **Conditional Expressions:**
* *Current:* `CMP`, `JE`, `LABEL`, `GOTO`
* *Future:* `if (a > b) { ... } else { ... }`
* Your compiler just needs to auto-generate the jump labels for you.


* [ ] **Logical Operators (`&&`, `||`, `!`):** These are essential for writing readable system code (e.g., `if (file_exists && is_dir)`).

### 3. The "Library" Layer (Standardization)

* [ ] **The `.include` Directive:** You need to stop copy-pasting your `draw_menu` logic. Create a `std.inc` file that holds your common GUI/Syscall macros and include it at the top of your scripts.
* [ ] **Pre-defined Syscall Wrappers:** Instead of manually loading `EAX` with `9` and triggering `INT 0x80`, have the compiler map `print_num(val, x, y, col)` to that assembly block automatically.

### 4. Memory Safety & Debugging

* [ ] **Runtime Bounds Checking:** Add a compile-time flag to insert "Safety Checks" into your `READBYTE` or array-access commands. It makes your code slightly slower but makes debugging 100x faster.
* [ ] **Symbol Table for Debugging:** Currently, if your app crashes, you have to look at hex. If your assembler generates a `.sym` file (mapping label names to addresses), your Exception Handler can print: `CRASH in task: menu.bin at label 'read_loop'`.

---

### Priority Implementation Roadmap

1. **`#include`:** It solves the code-bloat problem immediately.
2. **Stack Frames (`EBP` + Local Variables):** It solves the "Variable Corruption" problem (where `spin.txt` and `menu.txt` overwrite each other).
3. **Shunting-Yard Parser:** It solves the "Assembly-is-tedious" problem.
4. **`STRUCT` Support:** It solves the "Memory Management is hard" problem.
