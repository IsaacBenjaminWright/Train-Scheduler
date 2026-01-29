P2 – mts

I have made a working train schedule as the assignment desc. describes, which takes one txt file as input. my design as outlined in part 1 uses one mutex for simplicity and has a thread per train as well as a main thread and a dispatcher thread.

Compile with:
 make

Run with:
 ./mts <input.txt>

The rules are as the assignemnt specs follow:

Only one train on track at a time
High priority before low
If same priority:
Same direction means earliest ready first
 Opposite directions means opposite of last train
After two same direction, the opposite direction gets to go

1 mutex protects shared data.
3 condition variables: cv_loaded, cv_east, cv_west
Dispatcher signals trains according to scheduling rules