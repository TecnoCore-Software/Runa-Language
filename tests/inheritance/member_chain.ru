class (A) [
fun fa() [
return 1
]
]
class (B : A) [
fun fb() [
return self.fa() + 2
]
]
class (C : B) [
fun fc() [
return self.fb() + 3
]
]
c = new C()
printr(c.fc())
