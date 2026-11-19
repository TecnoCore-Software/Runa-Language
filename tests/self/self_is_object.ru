class (C) [
fun whoami() [
return self
]
]
a = new C()
b = a.whoami()
if a === b [
printr("self is the object")
]
