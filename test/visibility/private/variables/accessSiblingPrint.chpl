module M1 {
  private var foo = 10;
}

module M2 {
  use M1;
  proc main() {
    // ignoreInternalModules=true, ignoreBuiltInModules=true
    __primitive("get visible symbols", true, true);
    // Ensures that the private module level variable foo is not visible
    // via explicit naming.
  }
}
