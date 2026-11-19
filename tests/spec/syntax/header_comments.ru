#/ a comment may sit at the end of EVERY header line
if 2 > 1 #/ if header
[
printr("if")
] elif 2 > 1 #/ elif header
[
printr("elif")
] else #/ else header
[
printr("else")
]

fun greet(name) #/ fun header
[
return name
]

s = 0
for i = 1 to 2 #/ for header
[
s = s + i
]

loop s < 2 #/ loop header
[
s = s + 1
]

class (A) #/ class header
[
x = 0

constructor(x) #/ constructor header
[
self.x = x
]

fun get() #/ method header
[
return self.x
]
]

printr(greet("rona"))
printr(s)
printr(new A(9).get())
