# Python: add 10 cents ten times
total = 0.0
for i in range(10):
    total = total + 0.10
print("Total:", total)
print("Is it R1.00?", total == 1.0)

flights = 12500.00   # rand
hotel = 850.00       # dollars!
print("Trip:", flights + hotel)
