#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

//define enums for priority and direction
typedef enum {
    EAST = 0,
    WEST = 1,
    NO_DIR = 2
} Direction;

typedef enum {
    LOW  = 0,
    HIGH = 1
} Priority;

typedef struct Train {
    int         id;
    Direction   dir;
    Priority    pri;
    int         load_time;     // in tenths of a second, not seconds
    int         cross_time;    
    long long   ready_time; // two longs create our ready time
} Train;

// quueus node
typedef struct QNode {
    Train          *t;
    struct QNode   *next;
} QNode;

// A queue ordered by the specs of assignment, ready and id
typedef struct {
    QNode *head;
} Queue;

//condition variables protected by single mtx
pthread_mutex_t mtx        = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cv_loaded  = PTHREAD_COND_INITIALIZER;
pthread_cond_t  cv_east    = PTHREAD_COND_INITIALIZER;
pthread_cond_t  cv_west    = PTHREAD_COND_INITIALIZER;

// four queus
Queue q_east_high = { NULL };
Queue q_east_low  = { NULL };
Queue q_west_high = { NULL };
Queue q_west_low  = { NULL };

FILE *out_fp = NULL;

bool track_free       = true;
int  selected_id      = -1;
int  trains_left = 0;

Direction last_dir         = NO_DIR;
int       run_len = 0;
bool      turn    = false;

struct timespec start_time;

#define MAX_TRAINS 256
Train     trains[MAX_TRAINS];
pthread_t train_threads[MAX_TRAINS], dispatcher_thread;
int       num_trains = 0;

//time managment, now and since start
static long long now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static long long since_start(void) {
    long long cur  = now();
    long long base = (long long)start_time.tv_sec * 1000000000LL + start_time.tv_nsec;
    return cur - base;
}

//time printing

static const char *dir_str(Direction d) {
    return (d == EAST) ? "East" : "West";
}

static void print_time(FILE *fp) {
    long long ns = since_start();
    long long tenths = ns / 100000000LL; // 1e8
    long long total_seconds = tenths / 10;
    int tenth_digit = (int)(tenths % 10);

    int hours   = (int)(total_seconds / 3600);
    int minutes = (int)((total_seconds % 3600) / 60);
    int seconds = (int)(total_seconds % 60);

    fprintf(fp, "%02d:%02d:%02d.%d ", hours, minutes, seconds, tenth_digit);
}


static void print_ready(Train *t) {
    // cmd line
    print_time(stdout);
    fprintf(stdout, "Train %2d is ready to go %4s\n", t->id, dir_str(t->dir));
    fflush(stdout);

    // output.txt
    if (out_fp) {
        print_time(out_fp);
        fprintf(out_fp, "Train %2d is ready to go %4s\n", t->id, dir_str(t->dir));
        fflush(out_fp);
    }
}

static void print_on(Train *t) {
    // cmd line
    print_time(stdout);
    fprintf(stdout, "Train %2d is ON the main track going %4s\n", t->id, dir_str(t->dir));
    fflush(stdout);

    // output.txt
    if (out_fp) {
        print_time(out_fp);
        fprintf(out_fp, "Train %2d is ON the main track going %4s\n", t->id, dir_str(t->dir));
        fflush(out_fp);
    }
}

static void print_off(Train *t) {
    // cmd line
    print_time(stdout);
    fprintf(stdout, "Train %2d is OFF the main track after going %4s\n", t->id, dir_str(t->dir));
    fflush(stdout);

    // output.txt
    if (out_fp) {
        print_time(out_fp);
        fprintf(out_fp, "Train %2d is OFF the main track after going %4s\n", t->id, dir_str(t->dir));
        fflush(out_fp);
    }
}

//q helpers, empty, look, insert uses

static bool q_empty(Queue *q) { return q->head == NULL; }

static Train *q_look(Queue *q) {
    return q->head ? q->head->t : NULL;
}


static void q_insert(Queue *q, Train *t) {
    QNode *n = (QNode *)malloc(sizeof(QNode));
    n->t = t; n->next = NULL;


    if (!q->head) {
        q->head = n;
        return;
    }

    QNode *prev = NULL, *cur = q->head;
    while (cur) {
        long a_rt = cur->t->ready_time;
        long b_rt = t->ready_time;
        if (b_rt < a_rt) break;
        if (b_rt == a_rt && t->id < cur->t->id) break;
        prev = cur;
        cur = cur->next;
    }
    // insert at the head
    if (!prev) {           
        n->next = q->head;
        q->head = n;
    // insert after the prev
    } else {               
        n->next = prev->next;
        prev->next = n;
    }
}
//pop the head node, hope to god it exists
static Train *q_pop(Queue *q) {
    if (!q->head){
        return NULL;
    } 
    QNode *n = q->head;
    Train *t = n->t;
    q->head = n->next;
    free(n);
    return t;
}

//station q
static inline Queue *queue_for(Direction d, Priority p) {
    if (d == EAST && p == HIGH){
        return &q_east_high;
    } 
    if (d == EAST && p == LOW){
        return &q_east_low;
    } 
    if (d == WEST && p == HIGH){
        return &q_west_high;
    } else{
        return &q_west_low;
    }
    
}

static bool has_any_dir(Direction d) {
    return !q_empty(queue_for(d, HIGH)) || !q_empty(queue_for(d, LOW));
}

static Direction opposite(Direction d) {
    return (d == EAST) ? WEST : EAST;
}

static bool any_train_ready(void) {
    return has_any_dir(EAST) || has_any_dir(WEST);
}

//direction fairness, make sure that we are getting both directions
static void direction_fairness(Direction dir) {
    if (last_dir == dir) {
        run_len++;
    } else {
        last_dir = dir;
        run_len = 1;
    }
    // if 2 ormore and opposite waiting, schedule the opposite next
    if (run_len >= 2 && has_any_dir(opposite(dir))) {
        turn = true;
    }
}

//choosing next train, holds the mtx
static Train *pick_from_dir(Direction d) {
    Queue *qh = queue_for(d, HIGH);
    Queue *ql = queue_for(d, LOW);
    if (!q_empty(qh)) return q_look(qh);
    if (!q_empty(ql)) return q_look(ql);
    return NULL;
}

static Train *choose_next_train(void) {
    //if turn, force opposite of last_dir
    if (turn && last_dir != NO_DIR) {
        Direction must = opposite(last_dir);
        Train *candidate = pick_from_dir(must);
        if (candidate) {
            turn = false;
            return candidate;
        }
        // If no candidat, clear
        turn = false;
    }

    //High first
    bool eastHigh = !q_empty(queue_for(EAST, HIGH));
    bool westHigh = !q_empty(queue_for(WEST, HIGH));
    bool anyHigh  = eastHigh || westHigh;

    if (anyHigh) {
        if (eastHigh && westHigh) {
            
            Direction prefer;
            //default
            if (last_dir == NO_DIR){
                prefer = WEST; 
            } 
            else{
                prefer = opposite(last_dir);
            }                      
            return q_look(queue_for(prefer, HIGH));
        } else if (eastHigh) {
            return q_look(queue_for(EAST, HIGH));
        } else {
            return q_look(queue_for(WEST, HIGH));
        }
    }

    // if no high, consider low
    bool eastLow = !q_empty(queue_for(EAST, LOW));
    bool westLow = !q_empty(queue_for(WEST, LOW));
    if (eastLow && westLow) {
        Direction prefer;
        if (last_dir == NO_DIR){
            prefer = WEST;
        } 
        else  {
            prefer = opposite(last_dir);
        }                    
        return q_look(queue_for(prefer, LOW));
    } else if (eastLow) {
        return q_look(queue_for(EAST, LOW));
    } else if (westLow) {
        return q_look(queue_for(WEST, LOW));
    }

    return NULL;
}

//now we can remove selected train
static void remove_from_queue_head(Train *t) {
    Queue *q = queue_for(t->dir, t->pri);
    Train *p = q_pop(q);
    (void)p;
}

static void *train_thread(void *arg) {
    Train *t = (Train *)arg;

    // Load time
    usleep(t->load_time * 100000);
    t->ready_time = since_start();

    //ready
    print_ready(t);

    // enqueue and wait
    pthread_mutex_lock(&mtx);

    q_insert(queue_for(t->dir, t->pri), t);
    pthread_cond_signal(&cv_loaded);

    while (selected_id != t->id) {
        if (t->dir == EAST){
            pthread_cond_wait(&cv_east, &mtx);
        } 
        else {
            pthread_cond_wait(&cv_west, &mtx);
        }                   
    }

    pthread_mutex_unlock(&mtx);

    // cross time
    print_on(t);
    usleep(t->cross_time * 100000);

    pthread_mutex_lock(&mtx);
    print_off(t);
    track_free = true;
    selected_id = -1;         
    trains_left--;
    pthread_cond_signal(&cv_loaded);
    pthread_mutex_unlock(&mtx);

    return NULL;
}

static void *dispatcher_thread_fn(void *arg) {
    (void)arg;
    pthread_mutex_lock(&mtx);

    while (trains_left > 0) {
        // wait for ready and free track
        while ((!any_train_ready() || !track_free) && trains_left > 0) {
            pthread_cond_wait(&cv_loaded, &mtx);
        }
        if (trains_left <= 0) break;

        Train *chosen = choose_next_train();
        selected_id = chosen->id;
        track_free = false;

        // remove from q and update the direction_fairness
        remove_from_queue_head(chosen);
        direction_fairness(chosen->dir);

        if (chosen->dir == EAST) {
            pthread_cond_broadcast(&cv_east);
        }
        else {
            pthread_cond_broadcast(&cv_west);
        }                   
    }
    
    pthread_mutex_unlock(&mtx);
    return NULL;
}

//parses an input txt file of format in the assignment specs
static int parse_input(const char *fname) {
    FILE *fp = fopen(fname, "r");
    if (!fp) { 
        perror("fopen"); 
        exit(1); 
        }

    int tid = 0;
    while (tid < MAX_TRAINS) {
        char dir;
        int load, cross;
        int rv = fscanf(fp, " %c %d %d", &dir, &load, &cross);
        if (rv != 3) break;

        trains[tid].id         = tid;
        trains[tid].load_time  = load;
        trains[tid].cross_time = cross;
        trains[tid].ready_time = 0;

        switch (dir) {
            case 'e': trains[tid].dir=EAST; 
                trains[tid].pri=LOW;  
                break;
            case 'E': trains[tid].dir=EAST; 
                trains[tid].pri=HIGH; 
                break;
            case 'w': trains[tid].dir=WEST; 
                trains[tid].pri=LOW;  
                break;
            case 'W': trains[tid].dir=WEST; 
                trains[tid].pri=HIGH; 
                break;
            default:
                fprintf(stderr, "Invalid direction char '%c'\n", dir);
                fclose(fp);
                exit(1);
        }
        tid++;
    }

    fclose(fp);
    return tid;
}


int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <inputfile>\n", argv[0]);
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &start_time);

    out_fp = fopen("output.txt", "w");
        if (!out_fp) {
            perror("fopen output.txt");
            exit(1);
        }

    num_trains = parse_input(argv[1]);
    trains_left = num_trains;

    //dispatcher thread
    if (pthread_create(&dispatcher_thread, NULL, dispatcher_thread_fn, NULL) != 0) {
        perror("pthread_create dispatcher");
        exit(1);
    }
    //train threads
    for (int i = 0; i < num_trains; i++) {
        if (pthread_create(&train_threads[i], NULL, train_thread, &trains[i]) != 0) {
            perror("pthread_create train");
            exit(1);
        }
    }
    for (int i = 0; i < num_trains; i++){
        pthread_join(train_threads[i], NULL);
    } 

    // error for dispatcher not exiting properlry
    pthread_mutex_lock(&mtx);
    trains_left = 0;
    pthread_cond_signal(&cv_loaded);
    pthread_mutex_unlock(&mtx);

    pthread_join(dispatcher_thread, NULL);

    fclose(out_fp);
    return 0;

}