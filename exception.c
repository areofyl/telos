void print(const char *s);
void print_hex(unsigned long val);

// just dump the exception info and hang
void exception_handler(unsigned long esr, unsigned long elr,
                       unsigned long far) {
  print("EXCEPTION\n");
  print("   ESR: ");
  print_hex(esr);
  print("\n   ELR: ");
  print_hex(elr);
  print("\n   FAR: ");
  print_hex(far);
  print("\n");

  for (;;) {
  }
}
