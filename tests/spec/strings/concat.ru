#/ String concatenation with '+' (DECISIONS #128): a string on either side
#/ glues text; every other operand contributes exactly its printr form.
printr("Hello!" + "Welcome!")
first = "Runa"
second = "language"
printr(first + " " + second)
printr("n=" + 5)
printr(7 + "!")
printr("f=" + 1.5)
printr("b=" + true)
printr("x=" + null)
printr("" + "stays itself")
printr("a" + "b" + "c")
