class (Inventory) [
    items = {}
    fun total() [
        return sum(self.items)
    ]
]
item_box = new Inventory()
item_box.items = {5, 10, 15}
printr(item_box.total())
printr(contains(item_box.items, 10))
