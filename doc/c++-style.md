### C/C++ Style

- All block statements (`if`, `else`, `for`, `while`, `switch`) must use braces, even for single-line bodies.
- It's not necessary to translate C style to the style of Verilog "begin" and "end" , as Verilog's "begin" and "end" are words and don't key visually the same way.

```c
if (condition) {
    do_thing();
} else {
    do_other_thing();
}

void foo(void)
{
    bar();
}
```

