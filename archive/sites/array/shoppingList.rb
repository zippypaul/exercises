@completeShoppingList = []


@secondShoppingList = ["cat","dog"]

def retrieveItemsToPurchase
  @purchaseItem = $stdin.gets.chomp
  @completeShoppingList << @purchaseItem
  #puts @completeShoppingList
end



while @completeShoppingList.length < 3  do retrieveItemsToPurchase
end

@completeShoppingList.each do |poo|



puts "Next shopping item is #{poo}"

end
@completeShoppingList << @secondShoppingList
puts @completeShoppingList

puts @completeShoppingList.length
