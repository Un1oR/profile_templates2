////////////////////////////////////////////////////////////////////////////////
///
/// postprocess.cpp
/// ---------------
///
/// Copyright (c) 2008-2009 Steven Watanabe
/// Copyright (c) 2011-2013 Domagoj Saric
///
///  Use, modification and distribution is subject to the Boost Software License, Version 1.0.
///  (See accompanying file LICENSE_1_0.txt or copy at
///  http://www.boost.org/LICENSE_1_0.txt)
///
/// For more information, see http://www.boost.org
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#undef BOOST_ENABLE_ASSERT_HANDLER

#define BOOST_DATE_TIME_NO_LIB
#define BOOST_XPRESSIVE_USE_C_TRAITS

#include "postprocess.hpp"

#include "boost/xpressive/xpressive.hpp"
#include "boost/foreach.hpp"
#include "boost/lexical_cast.hpp"
#include "boost/numeric/conversion/cast.hpp"
#include "boost/ptr_container/ptr_vector.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------

namespace expressions
{
    using namespace boost::xpressive;
    #ifdef _MSC_VER

        #pragma warning(disable:4512)

        sregex const enter_message      ( (s1= *_) >> " : warning C4150: deletion of pointer to incomplete type 'template_profiler::incomplete_enter'; no destructor called" );
        sregex const exit_message       ( (s1= *_) >> " : warning C4150: deletion of pointer to incomplete type 'template_profiler::incomplete_exit'; no destructor called"  );
        sregex const call_graph_line    ( "        " >> (s1= *_) >> '(' >> ( s2= +_d ) >> ')' >> " : see reference to " >> *_ );
        sregex const split_file_and_line( (s1= *_) >> '(' >> ( s2= +_d ) >> ')' );

    #elif defined(__GNUC__)

        #if (__GNUC__ < 4) || (__GNUC_MINOR__ < 3)

            sregex const enter_message      (sregex::compile( "(.*): warning: division by zero in .template_profiler::enter_value / 0."));
            sregex const exit_message       (sregex::compile( "(.*): warning: division by zero in .template_profiler::exit_value / 0." ));
            sregex const call_graph_line    (sregex::compile( "(.*):(\\d+):   instantiated from .*"                                    ));
            sregex const split_file_and_line(sregex::compile( "(.*):(\\d+)"                                                            ));

        #else

            sregex const enter_message      (sregex::compile( "(.*): warning: .+int template_profiler::enter\\(int\\).*"));
            sregex const exit_message       (sregex::compile( "(.*): warning: .+int template_profiler::exit\\(int\\).*" ));
            sregex const call_graph_line    (sregex::compile( "(.*):(\\d+):   instantiated from .*"                     ));
            sregex const split_file_and_line(sregex::compile( "(.*):(\\d+)"                                             ));

        #endif

    #else

        #error Unknown Compiler

    #endif
} // namespace expressions

std::ofstream output;

struct print {
    int* cummulative;
    int width;
    typedef void result_type;
    template<class T>
    void operator()(const T& t) {
        *cummulative += t.second;
        output << std::setw(width) << t.first << std::setw(10) << t.second << std::setw(10) << *cummulative << std::endl;
    }
};

struct compare {
    template<class T>
    bool operator()(const T& lhs, const T& rhs) const {
        return(lhs.second > rhs.second);
    }
};

typedef std::pair<std::string, int> line_id;

struct node_info {
    node_info() : count(0), total_with_children(0) {}
    std::map<const line_id*, int> children;
    std::map<const line_id*, int> parents;
    int count;
    int total_with_children;
};

struct call_graph_less {
    template<class T>
    bool operator()(const T& lhs, const T& rhs) const {
        return(lhs.second.total_with_children > rhs.second.total_with_children);
    }
};

struct print_line_id {
    print_line_id(const line_id* x) : line(x) {}
    const line_id* line;
};

std::ostream& operator<<(std::ostream& os, const print_line_id& arg) {
    os << arg.line->first << '(' << arg.line->second << ')';
    return(os);
}

class instantiation_state {
public:
    instantiation_state() : current(&root) {}
    void finish_instantiation() {
        // be at least somewhat resilient to errors
        if(current != &root) {
            current = current->up;
        }
    }
    void add_instantiation(const line_id* new_line, std::size_t /*backtrace_size*/) {
        // don't try to deal with metafunction forwarding
        node* child(new node(new_line, current));
        current->children.push_back(child);
        current = child;
    }
    void get_graph(std::map<const line_id*, node_info>& graph) const {
        get_graph_impl(graph, &root);
    }
private:
    static void add_child(std::map<const line_id*, node_info>& graph, const line_id* parent, const line_id* child) {
        if(parent && child && parent != child) {
            ++graph[parent].children[child];
            ++graph[child].parents[parent];
            ++graph[parent].total_with_children;
        }
    }
    struct node {
        node(const line_id* i = 0, node* u = 0) : id(i), up(u), depth(up?up->depth+1:0) {
        }
        boost::ptr_vector<node> children;
        const line_id* id;
        node* up;
        int depth;
    };
    static void get_graph_impl(std::map<const line_id*, node_info>& graph, const node* root) {
        BOOST_FOREACH(const node& child, root->children) {
            get_graph_impl(graph, &child);
        }
        if(root->id != 0) {
            ++graph[root->id].count;
        }
        for(node* parent = root->up; parent != 0; parent = parent->up) {
            add_child(graph, parent->id, root->id);
        }
    }
    node* current;
    node root;
};


void postprocess
(
    char const * const input_file_name,
    char const * const output_file_name
)
{
    bool const use_call_graph( true );

    output.open( output_file_name, std::ios_base::trunc );

    std::map<std::string, int> messages;
    std::string line;
    int total_matches = 0;
    int max_match_length = 0;
    {
        std::ifstream input(input_file_name);
        while(std::getline(input, line)) {
            boost::xpressive::smatch match;
            if(boost::xpressive::regex_match(line, match, expressions::enter_message)) {
                max_match_length = (std::max)(max_match_length, boost::numeric_cast<int>(match[1].length()));
                ++messages[match[1]];
                ++total_matches;
            }
        }
    }
    std::vector<std::pair<std::string, int> > copy(messages.begin(), messages.end());
    std::sort(copy.begin(), copy.end(), compare());
    output << "Total instantiations: " << total_matches << std::endl;
    int cummulative = 0;
    output << std::setw(max_match_length) << "Location" << std::setw(10) << "count" << std::setw(10) << "cum." << std::endl;
    output << std::setfill('-') << std::setw(max_match_length + 20) << '-' << std::setfill(' ') << std::endl;
    print p = { &cummulative, max_match_length };
    std::for_each(copy.begin(), copy.end(), p);

    if(use_call_graph) {
        std::size_t backtrace_depth = 0;
        std::map<const line_id*, node_info> graph;
        instantiation_state state;
        std::set<line_id> lines;
        typedef std::pair<std::string, int> raw_line;
        BOOST_FOREACH(const raw_line& l, messages) {
            boost::xpressive::smatch match;
            boost::xpressive::regex_match(l.first, match, expressions::split_file_and_line);
            lines.insert(line_id(match[1], boost::lexical_cast<int>(match[2].str())));
        }
        const line_id* current_instantiation = 0;
        std::ifstream input(input_file_name);
#if defined(_MSC_VER)
        // msvc puts the warning first and follows it with the backtrace.
        while(std::getline(input, line)) {
            boost::xpressive::smatch match;
            if(boost::xpressive::regex_match(line, match, expressions::enter_message)) {
                // commit the current instantiation
                if(current_instantiation) {
                    state.add_instantiation(current_instantiation, backtrace_depth);
                }
                // start a new instantiation
                std::string file_and_line(match[1].str());
                boost::xpressive::regex_match(file_and_line, match, expressions::split_file_and_line);
                std::string file = match[1];
                int line = boost::lexical_cast<int>(match[2].str());
                current_instantiation = &*lines.find(line_id(file, line));
            } else if(boost::xpressive::regex_match(line, match, expressions::call_graph_line)) {
                ++backtrace_depth;
            } else if(boost::xpressive::regex_match(line, match, expressions::exit_message)) {
                // commit the current instantiation
                if(current_instantiation) {
                    state.add_instantiation(current_instantiation, backtrace_depth);
                }

                state.finish_instantiation();
                if(backtrace_depth) {
                    --backtrace_depth;
                }
                current_instantiation = 0;
            }
        }
        // commit the current instantiation
        if(current_instantiation) {
            state.add_instantiation(current_instantiation, backtrace_depth);
        }
#elif defined(__GNUC__)
        // gcc puts the backtrace first, and then the warning.
        while(std::getline(input, line)) {
            boost::xpressive::smatch match;
            if(boost::xpressive::regex_match(line, match, expressions::enter_message)) {
                std::string file_and_line(match[1].str());
                boost::xpressive::regex_match(file_and_line, match, expressions::split_file_and_line);
                std::string file = match[1];
                int line = boost::lexical_cast<int>(match[2].str());
                current_instantiation = &*lines.find(line_id(file, line));
                ++backtrace_depth;
                std::cerr << backtrace_depth << std::endl;
                state.add_instantiation(current_instantiation, backtrace_depth);
                backtrace_depth = 0;
            } else if(boost::xpressive::regex_match(line, match, expressions::call_graph_line)) {
                ++backtrace_depth;
            } else if(boost::xpressive::regex_match(line, match, expressions::exit_message)) {
                state.finish_instantiation();
                backtrace_depth = 0;
            }
        }
#else
    #error Unsupported compiler
#endif
        state.get_graph(graph);
        typedef std::pair<const line_id*, node_info> call_graph_node_t;
        std::vector<call_graph_node_t> call_graph;
        std::copy(graph.begin(), graph.end(), std::back_inserter(call_graph));
        std::sort(call_graph.begin(), call_graph.end(), call_graph_less());
        output << "\nCall Graph\n\n";
        BOOST_FOREACH(const call_graph_node_t& node, call_graph) {
            output << print_line_id(node.first) << " (" << node.second.count << ")\n";
            typedef std::map<const line_id*, int>::const_reference node_t;
            output << "  Parents:\n";
            BOOST_FOREACH(node_t n, node.second.parents) {
                output << "    " << print_line_id(n.first) << " (" << n.second << ")\n";
            }
            output << "  Children:\n";
            BOOST_FOREACH(node_t n, node.second.children) {
                output << "    " << print_line_id(n.first) << " (" << n.second << "/" << graph[n.first].count << ")\n";
            }
        }
    }
}

//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
