shoppingListFood = [
  "eggs",
  "bread",
  "celery",
  "beans",
  "juice"
]

shoppingListClothes = [
  "tie",
  "bread",
  "shoes",
  "jeans",
  "hats"
]

amalgamatedList = []

shoppingListFood.each do |listFood|
amalgamatedList << listFood
end

shoppingListClothes.each do |listClothes|
amalgamatedList << listClothes
end

puts amalgamatedList.uniq
