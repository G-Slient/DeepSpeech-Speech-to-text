#include "path_trie.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "decoder_utils.h"

PathTrie::PathTrie() {
  log_prob_b_prev = -NUM_FLT_INF;
  log_prob_nb_prev = -NUM_FLT_INF;
  log_prob_b_cur = -NUM_FLT_INF;
  log_prob_nb_cur = -NUM_FLT_INF;
  log_prob_c = -NUM_FLT_INF;
  score = -NUM_FLT_INF;

  ROOT_ = -1;
  character = ROOT_;
  timestep = 0;
  exists_ = true;
  parent = nullptr;

  dictionary_ = nullptr;
  dictionary_state_ = 0;
  has_dictionary_ = false;

  matcher_ = nullptr;
}

PathTrie::~PathTrie() {
  for (auto child : children_) {
    delete child.second;
  }
}

PathTrie* PathTrie::get_path_trie(int new_char, int new_timestep, float cur_log_prob_c, bool reset) {
  auto child = children_.begin();
  for (child = children_.begin(); child != children_.end(); ++child) {
    if (child->first == new_char) {
      // If existing child matches this new_char but had a lower probability,
      // and it's a leaf, update its timestep to new_timestep.
      // The leak check makes sure we don't update the child to have a later
      // timestep than a grandchild.
      if (child->second->log_prob_c < cur_log_prob_c &&
          child->second->children_.size() == 0) {
        child->second->log_prob_c = cur_log_prob_c;
        child->second->timestep = new_timestep;
      }
      break;
    }
  }
  if (child != children_.end()) {
    if (!child->second->exists_) {
      child->second->exists_ = true;
      child->second->log_prob_b_prev = -NUM_FLT_INF;
      child->second->log_prob_nb_prev = -NUM_FLT_INF;
      child->second->log_prob_b_cur = -NUM_FLT_INF;
      child->second->log_prob_nb_cur = -NUM_FLT_INF;
    }
    return child->second;
  } else {
    if (has_dictionary_) {
      matcher_->SetState(dictionary_state_);
      bool found = matcher_->Find(new_char + 1);
      if (!found) {
        // Adding this character causes word outside dictionary
        auto FSTZERO = fst::TropicalWeight::Zero();
        auto final_weight = dictionary_->Final(dictionary_state_);
        bool is_final = (final_weight != FSTZERO);
        if (is_final && reset) {
          dictionary_state_ = dictionary_->Start();
        }
        return nullptr;
      } else {
        PathTrie* new_path = new PathTrie;
        new_path->character = new_char;
        new_path->timestep = new_timestep;
        new_path->parent = this;
        new_path->dictionary_ = dictionary_;
        new_path->has_dictionary_ = true;
        new_path->matcher_ = matcher_;
        new_path->log_prob_c = cur_log_prob_c;

        // set spell checker state
        // check to see if next state is final
        auto FSTZERO = fst::TropicalWeight::Zero();
        auto final_weight = dictionary_->Final(matcher_->Value().nextstate);
        bool is_final = (final_weight != FSTZERO);
        if (is_final && reset) {
          // restart spell checker at the start state
          new_path->dictionary_state_ = dictionary_->Start();
        } else {
          // go to next state
          new_path->dictionary_state_ = matcher_->Value().nextstate;
        }

        children_.push_back(std::make_pair(new_char, new_path));
        return new_path;
      }
    } else {
      PathTrie* new_path = new PathTrie;
      new_path->character = new_char;
      new_path->timestep = new_timestep;
      new_path->parent = this;
      new_path->log_prob_c = cur_log_prob_c;
      children_.push_back(std::make_pair(new_char, new_path));
      return new_path;
    }
  }
}

void PathTrie::get_path_vec(std::vector<int>& output, std::vector<int>& timesteps) {
  // Recursive call: recurse back until stop condition, then append data in
  // correct order as we walk back down the stack in the lines below.
  if (parent != nullptr) {
    parent->get_path_vec(output, timesteps);
  }
  if (character != ROOT_) {
    output.push_back(character);
    timesteps.push_back(timestep);
  }
}

PathTrie* PathTrie::get_prev_grapheme(std::vector<int>& output,
                                      std::vector<int>& timesteps)
{
  PathTrie* stop = this;
  if (character == ROOT_) {
    return stop;
  }
  // Recursive call: recurse back until stop condition, then append data in
  // correct order as we walk back down the stack in the lines below.
  //FIXME: use Alphabet instead of hardcoding +1 here
  if (!byte_is_codepoint_boundary(character + 1)) {
    stop = parent->get_prev_grapheme(output, timesteps);
  }
  output.push_back(character);
  timesteps.push_back(timestep);
  return stop;
}

int PathTrie::distance_to_codepoint_boundary(unsigned char *first_byte)
{
  //FIXME: use Alphabet instead of hardcoding +1 here
  if (byte_is_codepoint_boundary(character + 1)) {
    *first_byte = (unsigned char)character + 1;
    return 1;
  }
  if (parent != nullptr && parent->character != ROOT_) {
    return 1 + parent->distance_to_codepoint_boundary(first_byte);
  }
  assert(false); // unreachable
  return 0;
}

PathTrie* PathTrie::get_prev_word(std::vector<int>& output,
                                  std::vector<int>& timesteps,
                                  int space_id)
{
  PathTrie* stop = this;
  if (character == space_id || character == ROOT_) {
    return stop;
  }
  // Recursive call: recurse back until stop condition, then append data in
  // correct order as we walk back down the stack in the lines below.
  if (parent != nullptr) {
    stop = parent->get_prev_word(output, timesteps, space_id);
  }
  output.push_back(character);
  timesteps.push_back(timestep);
  return stop;
}

void PathTrie::iterate_to_vec(std::vector<PathTrie*>& output) {
  if (exists_) {
    log_prob_b_prev = log_prob_b_cur;
    log_prob_nb_prev = log_prob_nb_cur;

    log_prob_b_cur = -NUM_FLT_INF;
    log_prob_nb_cur = -NUM_FLT_INF;

    score = log_sum_exp(log_prob_b_prev, log_prob_nb_prev);
    output.push_back(this);
  }
  for (auto child : children_) {
    child.second->iterate_to_vec(output);
  }
}

void PathTrie::remove() {
  exists_ = false;

  if (children_.size() == 0) {
    for (auto child = parent->children_.begin(); child != parent->children_.end(); ++child) {
      if (child->first == character) {
        parent->children_.erase(child);
        break;
      }
    }

    if (parent->children_.size() == 0 && !parent->exists_) {
      parent->remove();
    }

    delete this;
  }
}

void PathTrie::set_dictionary(std::shared_ptr<PathTrie::FstType> dictionary) {
  dictionary_ = dictionary;
  dictionary_state_ = dictionary_->Start();
  has_dictionary_ = true;
}

void PathTrie::set_matcher(std::shared_ptr<fst::SortedMatcher<FstType>> matcher) {
  matcher_ = matcher;
}

#ifdef DEBUG
void PathTrie::vec(std::vector<PathTrie*>& out) {
  if (parent != nullptr) {
    parent->vec(out);
  }
  out.push_back(this);
}

void PathTrie::print(const Alphabet& a) {
  std::vector<PathTrie*> chain;
  vec(chain);
  std::string tr;
  printf("characters:\t ");
  for (PathTrie* el : chain) {
    printf("%X ", (unsigned char)(el->character));
    if (el->character != ROOT_) {
      tr.append(a.StringFromLabel(el->character));
    }
  }
  printf("\ntimesteps:\t ");
  for (PathTrie* el : chain) {
    printf("%d ", el->timestep);
  }
  printf("\n");
  printf("transcript:\t %s\n", tr.c_str());
}
#endif // DEBUG
