alphabet = [
  "a",  "b",  "c",  "d",
  "e",  "f",  "g",  "h",
  "i",  "j",  "k",  "l",
  "m",  "n",  "o",  "p",
  "q",  "r",  "s",  "t",
  "u",  "v",  "w",  "x",
  "y"
]

alphabet.push ["red", "green"]


firstletter = alphabet[0]

alphabet.each do |poo|

puts "This letter is #{poo}"

end

string = "this is my first string"
string2 = "this is my 2nd string"
string3 = string + string2

puts string3

something = "pear"
somethingElse = 50

# if something != somethingElse  #= means "does not equal"
  # if something <= somethingElse  #= means "greater than or equal to" or of course smaller than or equal to

# puts "do something"
# end


# GOOGLE = "George Boole"
# GOOGLE = "Boolean algebra"


case something
  when "pear"
    puts "something is a pear"
  when "apple"
    puts "somethign is an apple"
  when "banana"
    puts "something is a banana"
  else
    puts "that's not a fruit!!"
end
#print the length
puts alphabet.length

#this will print 20 years - ie: the range.
for year in 2000..2020
  puts year
end
