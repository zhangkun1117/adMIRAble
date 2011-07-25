#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ranker.hh"
#include "thread_safe_queue.hh"
#include <thread>
#include <future>
#include <utility>
#include <vector>
#include <string>

class input_thread {
    thread_safe_queue<std::pair<char*,std::promise<double>*>>* queue;
    ranker::predictor& model;
    std::thread thread;
    bool finished;

    public:
    void start() {
        while(!finished || !queue->empty()) {
            if(!queue->empty()) {
                std::pair<char*,std::promise<double>*> input = queue->atomic_pop();
                ranker::example x;
                char *inputstring = input.first;
                char *token = NULL; 
                token =  strsep(&inputstring, " \t"); // skip label
                for(;(token = strsep(&inputstring, " \t\n"));) {
                    if(!strcmp(token,"")) continue;
                    char* value = strrchr(token, ':');
                    if(value != NULL) {
                        *value = '\0';
                        double value_as_double = strtod(value + 1, NULL);
                        //nbe is the loss, not a feature
                        if(!strcmp(token, "nbe")) {
                            x.loss = value_as_double;
                        } else {
                            int location = strtol(token, NULL, 10);
                            x.features.push_back(ranker::feature(location, value_as_double));
                        }
                    }
                }
                double score = model.compute_score(x);
                input.second->set_value(score);
                free(input.first);
            } else {
                sched_yield();
            }
        }
    }

    void stop() {
        finished = true;
        thread.join();
    }

    input_thread(thread_safe_queue<std::pair<char*,std::promise<double>*>>* _queue, ranker::predictor& _model):queue(_queue),model(_model),finished(false) {
        thread = std::thread(&input_thread::start, this);
    }

};

class output_thread {
    std::thread thread;
    thread_safe_queue<std::vector<std::promise<double>*>*>* queue;
    bool finished;

    public:
    void start() {
        while(!finished || !queue->empty()) {
            if(!queue->empty()) {
                std::vector<std::promise<double>*>* result = queue->atomic_pop(); // single consumer
                int argmax = -1;
                double max = 0;
                for(size_t i = 0; i < result->size(); i++) {
                    double value = (*result)[i]->get_future().get();
                    if(argmax == -1 || value > max) {
                        argmax = i;
                        max = value;
                    }
                    delete (*result)[i];
                }
                queue->pop();
                delete result;
                fprintf(stdout, "%d\n", argmax);
            } else {
                sched_yield();
            }
        }
    }

    void stop() {
        finished = true;
        thread.join();
    }

    output_thread(thread_safe_queue<std::vector<std::promise<double>*>*>* _queue): queue(_queue), finished(false) {
        thread = std::thread(&output_thread::start, this);
    }
};

int main(int argc, char** argv) {
    if(argc < 3) {
        fprintf(stderr, "usage: %s <num-threads> <model>\n", argv[0]);
        return 1;
    }
    int num_threads = strtol(argv[1], NULL, 10);
    ranker::predictor model(1, std::string(argv[2]));
    thread_safe_queue<std::vector<std::promise<double>*>*> output_queue;
    thread_safe_queue<std::pair<char*, std::promise<double>*>> input_queue;
    output_thread output(&output_queue);
    std::vector<input_thread*> input;
    for(int i = 0; i < num_threads; i++) {
        input.push_back(new input_thread(&input_queue, model));
    }

    char* buffer = NULL;
    size_t buffer_length = 0;
    ssize_t length = 0;

    std::vector<std::promise<double>*>* results = new std::vector<std::promise<double>*>();
    while(0 <= (length = getline(&buffer, &buffer_length, stdin))) {
        if(length == 1) {
            output_queue.push(results);
            results = new std::vector<std::promise<double>*>();
        } else {
            std::promise<double>* result = new std::promise<double>();
            results->push_back(result);
            input_queue.push(std::pair<char*,std::promise<double>*>(strdup(buffer), results->back()));
        }
    }
    for(int i = 0; i < num_threads; i++) {
        input[i]->stop();
        delete input[i];
    }
    output.stop();
    return 0;
}