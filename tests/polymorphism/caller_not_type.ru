class (Animal) [
fun speak() [
printr("...")
]
]
class (Dog : Animal) [
override fun speak() [
printr("Woof")
]
]
class (Cat : Animal) [
override fun speak() [
printr("Meow")
]
]
zoo = new Dog()
zoo.speak()
zoo = new Cat()
zoo.speak()
