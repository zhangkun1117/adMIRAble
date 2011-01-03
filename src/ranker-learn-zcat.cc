#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>

#include <cassert>

#include <getopt.h>


#include <thread>


#define CLIP 0.2
#define LOOP 10

static int verbose_flag = 0;

//TODO: remove
using namespace std;

void  print_help_message(char *program_name)
{
  fprintf(stderr, "%s usage: %s [options]\n", program_name, program_name);
  fprintf(stderr, "OPTIONS :\n");
  fprintf(stderr, "      --train,-s <file>           : training set file\n");
  fprintf(stderr, "      --dev,-d   <file>           : dev set file\n");
  fprintf(stderr, "      --test,-t  <file>           : test set file\n");
  fprintf(stderr, "      --clip,-c  <double>         : clip value (default is %f)\n", CLIP);
  fprintf(stderr, "      --iter,-i  <int>            : nb of iterations (default is %d)\n", LOOP);
  fprintf(stderr, "      --mode,-m  <train|predict>  : running mode\n");
    
  fprintf(stderr, "      -help,-h                    : print this message\n");
}

FILE* openpipe(const char* filename) {
  int fd[2];
  pid_t childpid;
  if(pipe(fd) == -1) {
    perror("openpipe/pipe");
    exit(1);
  }
  if((childpid = fork()) == -1) {
    perror("openpipe/fork");
    exit(1);
  }
  if(childpid == 0) {
    close(fd[0]);
    dup2(fd[1], STDOUT_FILENO);
    //	execlp("zcat", "zcat", "-c", filename, (char*) NULL);
    execlp("pigz", "pigz", "-f", "-d", "-c", filename, (char*) NULL);
    perror("openpipe/execl");
    exit(1);
  }
  close(fd[1]);
  FILE* output = fdopen(fd[0], "r");
  return output;
}

void read_example_line(char*& buffer, int& buffer_size, FILE*& fp)
{
  while(buffer[strlen(buffer) - 1] != '\n') {
    buffer_size *= 2;
    buffer = (char*) realloc(buffer, buffer_size);
    if(fgets(buffer + strlen(buffer), buffer_size - strlen(buffer), fp) == NULL) break;
  }
}


struct Example {
  double loss;
  double score;
  int label;
  unordered_map<int, double> features;

  Example() : loss(0.0), score(0.0), label(0), features() {};

  // for sorting examples
  struct example_ptr_desc_order 
  {
    bool operator()(const Example* i, const Example* j) {return (i->score > j->score);}
  };
};




int compute_num_examples(const char* filename,
                         std::unordered_map<std::string,int>& features)
{
  FILE* fp = openpipe(filename);
  if(!fp) return 0;
  
  int num = 0;
  
  int buffer_size = 1024;
  char* buffer = (char*) malloc(buffer_size);
  while(NULL != fgets(buffer, buffer_size, fp)) {
    //get a line
    read_example_line(buffer,buffer_size,fp);

    //if line is empty -> we've read all the examples
    if(buffer[0] == '\n') {
      ++num;
      fprintf(stderr, "%d\r", num);
    }
    else {
      char * inputstring = buffer;
      char *token = NULL; 
      if ((token =  strsep(&inputstring, " \t")) != NULL) {
        // we dont read the first token as it is the label
      } 

      for(;(token = strsep(&inputstring, " \t\n"));) {
	if(!strcmp(token,"")) continue;

        char* value = strrchr(token, ':');
        if(value != NULL) {
          *value = '\0';
          string token_as_string = token;
	  if (token_as_string != "nbe")
	    features.insert(std::make_pair(token_as_string, features.size()));

	  // if (token_as_string != "nbe")
	  //   if(features.insert(std::make_pair(token_as_string, features.size())).second)
	  //     fprintf(stderr, "adding feature %s\n", token_as_string.c_str());


	  //if(features.find(token_as_string) == features.end() && token_as_string != "nbe") {
	  //    fprintf(stderr, "adding feature %s\n", token_as_string.c_str());
	  //features[token_as_string] = features.size();
          //}
        }
      }
    }
    if(feof(fp)) break;
  }
  int status;
  wait(&status);
  free(buffer);
  fclose(fp);
  
  
  return num;
}


struct mira_operator
{
  double avgUpdate;

  double clip;
  std::vector<double> &weights;
  std::vector<double> &avgWeights;
  Example* oracle;

  mira_operator(int loop, int num_examples, int iteration, int num, double clip_,
		std::vector<double> &weights_, std::vector<double> &avgWeights_, 
		Example* oracle_)
    : 
    avgUpdate(double(loop * num_examples - (num_examples * ((iteration + 1) - 1) + (num + 1)) + 1)),
    //    avgUpdate(1),
    clip(clip_),
    weights(weights_), avgWeights(avgWeights_), oracle(oracle_)
  {};

  inline
  bool incorrect_rank(const Example * example) 
  {
    return example->score > oracle->score || 
      (example->score == oracle->score && example->loss > oracle->loss);
  }


  void operator()(Example * example)
  {
    //fprintf(stdout, "%g %g\n", example->score, example->loss);
    
    // skip the oracle -> useless update
    if(example == oracle) return;
    
    
    if(incorrect_rank(example)) {
      double alpha = 0.0;
      double delta = example->loss - oracle->loss - (oracle->score - example->score);
    
      //copy
      unordered_map<int, double> difference = oracle->features;
      double norm = 0;
      
      unordered_map<int, double>::iterator end = example->features.end();
      
      for(unordered_map<int, double>::iterator j = example->features.begin(); j != end; ++j) {
	double&ref = difference[j->first];
	ref -= j->second;
	norm += ref*ref;
      }

      delta /= norm;
      alpha += delta;
      if(alpha < 0) alpha = 0;
      if(alpha > clip) alpha = clip;
      double avgalpha = alpha * avgUpdate;

      //update weight vectors
      end = difference.end();
      for(unordered_map<int, double>::iterator j = difference.begin(); j != end; ++j) {
	weights[j->first] += alpha * j->second;
	avgWeights[j->first] += avgalpha * j->second;
      }
    }
  }
};


// create an example from a line 'label fts:val .... fts:val'
// side-effects : update size of weights and avgweights
// unknown features are *ignored*
Example * fill_example(char*& buffer, const std::unordered_map<std::string,int>& features,
                       std::vector<double>& weights)
{
  Example * example = new Example();

  // read a line and fill label/features

  char * inputstring = buffer;
  char *token = NULL; 
  if ((token =  strsep(&inputstring, " \t")) != NULL) {
    if(!strcmp(token, "1")) {
      example->label = 1;
    }
  } 

  for(;(token = strsep(&inputstring, " \t\n"));) {
    if(!strcmp(token,"")) continue;
    
    char* value = strrchr(token, ':');
    if(value != NULL) {
      *value = '\0';
      string token_as_string = token;
      double value_as_double = strtod(value + 1, NULL);

      assert(!isinf(value_as_double));
      assert(!isnan(value_as_double));
      
      //nbe is the loss, not a feature
      if(token_as_string == "nbe") {
	example->loss = value_as_double;
      } 
      
      else {
	if(features.find(token_as_string) == features.end()) {
	  // fprintf(stderr, "could not find %s\n", token_as_string.c_str());
	  continue;
	}

	unsigned int location = features.at(token_as_string);
        
	example->features[location] = value_as_double;
	example->score += value_as_double * weights[location];
      }
    }
  }
  return example;
}


struct ExampleMaker
{
  std::thread my_thread;

  char* buffer;
  const std::unordered_map<std::string,int>& features;
  std::vector<double>& weights;

  Example * example;


  ExampleMaker(char* b, const std::unordered_map<std::string,int>& f, std::vector<double>& w)
    : buffer(b), features(f), weights(w), example(NULL) {};

  ~ExampleMaker() { free(buffer);}

  void join() {my_thread.join();}


  void create_example()
  {
    char * b = buffer;
    example = fill_example(b, features, weights)  ;
  }

  void start()
  {
    my_thread = std::thread(&ExampleMaker::create_example, this);
  }
  
};



int process(char* filename, int loop, vector<double> &weights, vector<double> &avgWeights, const unordered_map<string, int> &features, int iteration, 
	    int num_examples, double clip, bool alter_model) 
{
  int num = 0;
  int errors = 0;
  double avg_loss = 0;
  double one_best_loss = 0;
  
  std::vector<ExampleMaker*> examplemakers;

  


  FILE* fp = openpipe(filename);
  if(!fp) {
    fprintf(stderr, "ERROR: cannot open \"%s\"\n", filename);
    return 1;
  }

  int buffer_size = 1024;
  char* buffer = (char*) malloc(buffer_size);
  while(NULL != fgets(buffer, buffer_size, fp)) {
    
    //get a line
    read_example_line(buffer,buffer_size,fp);
    
    //if line is empty -> we've read all the examples
    if(buffer[0] == '\n') {

      //      fprintf(stderr, "converting examplemakers to examples\n");
      for(auto i = examplemakers.begin(); i != examplemakers.end(); ++i)
	(*i)->join();

      std::vector<Example*> examples(examplemakers.size(), NULL);
      Example* oracle = NULL;

      //      fprintf(stderr, "creating vectors of examples\n");
      //      fprintf(stderr, "retrieveing oracle \n");
      for(unsigned i = 0; i < examples.size(); ++i) {
	examples[i] = examplemakers[i]->example;
	if(oracle == NULL || examples[i]->loss < oracle->loss) 
	  oracle = examples[i];
      }
      
      //count the number of errors
      //fprintf(stdout, "num examples = %d\n", examples.size());
      
      // sort the examples by score
      one_best_loss += examples[0]->loss;
      sort(examples.begin(), examples.end(), Example::example_ptr_desc_order());
      avg_loss += examples[0]->loss;
      
      for(unsigned int i = 0; i < examples.size(); ++i) {
	if(examples[i]->score > oracle->score || (examples[i]->score == oracle->score && examples[i]->loss > oracle->loss)) {
	  ++errors;
	  break;
	}
      }
      
      // training -> update
      if(alter_model) {
	// std::for_each(examples.begin(),examples.end(), mira_operator(loop, num_examples, iteration, num, clip, 
        //                                                              weights, avgWeights, oracle));
	std::for_each(examples.begin(),examples.begin()+1, mira_operator(loop, num_examples, iteration, num, clip,
                                                                         weights, avgWeights, oracle));
      }

      ++num;
      if(num % 10 == 0) fprintf(stderr, "\r%d %d %f %f/%f", num, errors, (double)errors/num, avg_loss / num, one_best_loss / num);
            
      // reset data structures for next sentence
      oracle = NULL;

      for(unsigned i = 0; i < examples.size(); ++i) {
	delete examples[i];
	delete examplemakers[i];
      }
      //      examples.clear();
      examplemakers.clear();


      //fprintf(stdout, "\n");
      //if(num > 1000) break;
    }
    
    //read examples
    else{
      char * copy = strdup(buffer);
      ExampleMaker * em = new ExampleMaker(copy, features,weights);

      examplemakers.push_back(em);
      em->start();
    }
  }
  
  fprintf(stderr, "\r%d %d %f %f/%f\n", num, errors, (double)errors/num, avg_loss / num, one_best_loss / num);
  
  if(alter_model)
    // averaging for next iteration
    for(unsigned int i = 0; i < avgWeights.size(); ++i) {
      if(avgWeights[i] != 0.0)
	weights[i] = avgWeights[i] / (num_examples * loop);
    }

    // for(unsigned int i = 0; i < avgWeights.size(); ++i) {
    //   avgWeights[i] += weights[i];
    // }


  fclose(fp);
  int status;
  wait(&status);
  free(buffer);
  return 0;
}

int main(int argc, char** argv) {

  char * trainset = NULL;
  char * devset = NULL;
  char * testset = NULL;
  double clip = CLIP;
  int loop = LOOP;

  char mode_def[] = "train";
  char * mode = mode_def;


  // read the commandline
  int c;
  
  while(1) {
    
    static struct option long_options[] =
      {
	/* These options set a flag. */
	{"verbose", no_argument,       &verbose_flag, 1},
	/* These options don't set a flag.
	   We distinguish them by their indices. */
	{"help",        no_argument,             0, 'h'},
	{"train",       required_argument,       0, 's'},
	{"dev",         required_argument,       0, 'd'},
	{"test",        required_argument,       0, 't'},
	{"clip",        required_argument,       0, 'c'},
	{"iterations",  required_argument,       0, 'i'},
        //	{"mode",        required_argument,       0, 'm'},
	{0, 0, 0, 0}
      };
    // int to store arg position
    int option_index = 0;
    
    c = getopt_long (argc, argv, "s:d:t:c:i:m:hv", long_options, &option_index);

    // Detect the end of the options
    if (c == -1)
      break;
     
    switch (c)
      {
      case 0:
	// If this option set a flag, do nothing else now.
	if (long_options[option_index].flag != 0)
	  break;
	fprintf(stderr, "option %s", long_options[option_index].name);
	if (optarg)
	  fprintf(stderr, " with arg %s", optarg);
	fprintf (stderr, "\n");
	break;
     

      case 'h':
	print_help_message(argv[0]);
	exit(0);

      case 's':
	if(trainset) fprintf (stderr, "redefining ");
	fprintf (stderr, "training set filename: %s\n", optarg);
	trainset = optarg;
	break;
     
      case 'd':
	if(devset) fprintf (stderr, "redefining ");
	fprintf (stderr, "dev set filename: %s\n", optarg);
	devset = optarg;
	break;

      case 't':
	if(testset) fprintf (stderr, "redefining ");
	fprintf (stderr, "test set filename: %s\n", optarg);
	testset = optarg;
	break;


      case 'c':
	fprintf (stderr, "clip value: %s\n", optarg);
	clip = strtod(optarg, NULL);
	break;

      case 'i':
	fprintf (stderr, "number of iterations: %s\n", optarg);
	loop = atoi(optarg);
	break;

      // case 'm':
      //   fprintf (stderr, "mode is: %s (but I don't care ;)\n", optarg);
      //   mode = optarg;
      //   break;

      case '?':
	// getopt_long already printed an error message.
	break;
     
      default:
	abort ();
      }

  }


  if( trainset == NULL /* && !strcmp(mode, "train") */) {
    fprintf(stderr, "training mode and no trainset ? Aborting\n");
    abort();
  }  
    unordered_map<string, int> features;

    features.rehash(10*4659276);

    features.max_load_factor(0.5);

  

    int num_examples = compute_num_examples(trainset, features);

    fprintf(stderr, "Number of features: %lu\n", features.size());

    //    features.rehash(10*features.size());

    vector<double> weights(features.size(), 0.0);
    vector<double> avgWeights(features.size(), 0.0);


    fprintf(stderr, "examples: %d\n", num_examples);
    for(unsigned iteration = 0; iteration < unsigned(loop); ++iteration) {
      fprintf(stderr, "iteration %d\n", iteration);
      process(trainset, loop, weights, avgWeights, features, iteration, num_examples, clip, true);
      if(devset)
        process(devset, loop, weights, avgWeights, features, iteration, num_examples, clip, false);
      if(trainset) 
        process(testset, loop, weights, avgWeights, features, iteration, num_examples, clip, false);
    }

    unordered_map<string, int>::iterator end = features.end();
    // for( unordered_map<string, int>::iterator i = features.begin(); i != end; ++i) {
    //   if(avgWeights[i->second] != 0) {
    // 	fprintf(stdout, "%s 0 %32.31g\n", i->first.c_str(), avgWeights[i->second] / loop);
    //   }
    // }
    for( unordered_map<string, int>::iterator i = features.begin(); i != end; ++i) {
      if(weights[i->second] != 0) {
	fprintf(stdout, "%s 0 %32.31g\n", i->first.c_str(), weights[i->second] );
      }
    }
    return 0;
}
