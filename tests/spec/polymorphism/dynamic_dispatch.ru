class (Animal) [
    fun speak() [
        printr("...")
    ]
]
class (Dog : Animal) [
    override fun speak() [
        printr("woof")
    ]
]
a = new Dog()
a.speak()
