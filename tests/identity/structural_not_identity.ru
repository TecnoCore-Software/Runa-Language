class (Car) [
name = "same"
]
a = new Car()
b = new Car()
if a == b [
printr("structurally equal")
]
if a === b [
printr("identical")
] else [
printr("but not identical")
]
