@username = ARGV.first  #this gets the name of the user.


def get_numbers
  print "what's your first number?"
  @first_number = $stdin.gets.chomp.to_i
  print "what's your second number"
  @second_number = $stdin.gets.chomp.to_i
end

def calculator_subtract
  get_numbers
  print "let's do subtraction"
  puts
  answer = @first_number - @second_number
  puts "look #{@username} the answer = #{answer}"
end

def calculator_add
  get_numbers
  puts "let's do addition"
  puts
  answer = @first_number + @second_number
  puts "look #{@username} the answer = #{answer}"
end

def ask_me_what_to_calc
print "hello #{@username}, what type of calc? (a) add or (s) subtract:"
type_of_calc = $stdin.gets.chomp
if type_of_calc == "s"
  calculator_subtract
elsif type_of_calc == "a"
  calculator_add
else
  puts "shitty input sorry, try again"
end



ask_me_what_to_calc
end
total
