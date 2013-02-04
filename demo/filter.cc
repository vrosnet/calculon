/* Calculon © 2013 David Given
 * This code is made available under the terms of the Simplified BSD License.
 * Please see the COPYING file for the full license text.
 */

#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <math.h>
#include <boost/program_options.hpp>
#include "libnoise/noise.h"

#include "calculon.h"

using std::string;
namespace po = boost::program_options;

static bool readnumber(double& d)
{
	string s;
	if (!(std::cin >> s))
		return false;

	const char* p = s.c_str();
	char* endp;
	d = strtod(p, &endp);
	if (*endp)
	{
		std::cerr << "filter: malformed number in input data";
		exit(1);
	}

	return true;
}

int main(int argc, const char* argv[])
{
	po::options_description options("Allowed options");
	options.add_options()
	    ("help,h",
	    		"produce help message")
	    ("file,f",   po::value<string>(),
	    		"input Calculon script name")
	    ("script,s", po::value<string>(),
	    		"literal Calculon script")
	    ("precision,p", po::value<string>()->default_value("double"),
	    		"specifies whether to use double or float precision")
   		("dump,d",
   				"dump LLVM bitcode after compilation")
   	;

	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, options), vm);
	po::notify(vm);

	if (vm.count("help"))
	{
		std::cout << "filter: reads a list of numbers from stdin, processes each one with a\n"
				     "Calculon script, and writes them to stdout.\n"
		          << options <<
		             "\n"
		             "Try: echo 1 | filter --script 'sin(n)'\n";

		exit(1);
	}

	if (!vm.count("file") && !vm.count("script"))
	{
		std::cerr << "filter: you must specify the Calculon script to use!\n"
				     "(try --help)\n";
		exit(1);
	}

	if (vm.count("file") && vm.count("script"))
	{
		std::cerr << "filter: you can't specify *both* a file and a literal script!\n"
			     "(try --help)\n";
		exit(1);
	}

	std::istream* codestream;
	if (vm.count("file"))
	{
		string scriptfilename = vm["file"].as<string>();
		codestream = new std::ifstream(scriptfilename.c_str());
	}
	else
	{
		string script = vm["script"].as<string>();
		codestream = new std::stringstream(script);
	}
	bool dump = (vm.count("dump") > 0);
	string precision = vm["precision"].as<string>();

	if ((precision != "float") && (precision != "double"))
	{
		std::cerr << "filter: precision must be 'double' or 'float'\n"
				  << "(try --help)\n";
		exit(1);
	}

	if (precision == "double")
	{
		typedef Calculon::Instance<Calculon::RealIsDouble> Compiler;
		typedef Compiler::Real Real;
		typedef Compiler::Vector Vector;

		Compiler::StandardSymbolTable symbols;

		typedef Real TranslateFunction(Real n);
		Compiler::Program<TranslateFunction> func(symbols, *codestream, "(n)");
		if (dump)
			func.dump();

		double d;
		while (readnumber(d))
		{
			std::cout << func(d) << "\n";
		}
	}
	else
	{
		typedef Calculon::Instance<Calculon::RealIsFloat> Compiler;
		typedef Compiler::Real Real;
		typedef Compiler::Vector Vector;

		Compiler::StandardSymbolTable symbols;

		typedef Real TranslateFunction(Real n);
		Compiler::Program<TranslateFunction> func(symbols, *codestream, "(n)");
		if (dump)
			func.dump();

		double d;
		while (readnumber(d))
		{
			std::cout << func(d) << "\n";
		}
	}

	return 0;
}
