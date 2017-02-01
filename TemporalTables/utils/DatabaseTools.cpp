
#include "DatabaseTools.h"


const string DatabaseTools::dbName = "db.cpp";
const string DatabaseTools::dbNameCompiled = "db.so";
const string DatabaseTools::folderTmp = "tmp/";
const string DatabaseTools::folderTable = "../tbl/";
//Debug symbols: -g / Additional: -flto  -pipe
const string DatabaseTools::cmdBuild = "g++ -O3 -std=c++14 -fPIC -flto  -pipe %1% -shared -o %2%";


void DatabaseTools::split(const std::string& str, std::vector<std::string>& lineChunks) {
    std::stringstream in(str);
    std::string segment;
    lineChunks.clear();
    while (std::getline(in, segment, '|')) {
        lineChunks.push_back(segment);
    }
}

int DatabaseTools::compileFile(string name, string outname) {
    ifstream f(folderTmp + outname);
    if (f.good() && outname != dbNameCompiled) { // Only compile if not already on disk
        return 0;
    }

    boost::format command(cmdBuild);
    command % (folderTmp + name) % (folderTmp + outname);
    //cout << command << endl;
    return system(command.str().c_str());
}

Database* DatabaseTools::loadAndRunDb(string filename) {
    void* handle = dlopen((folderTmp + filename).c_str(), RTLD_NOW);
    if (!handle) {
        cerr << "error loading .so: " << dlerror() << endl;
        return nullptr;
    }

    auto make_database = reinterpret_cast<Database* (*)(string)>(dlsym(handle, "make_database"));
    if (!make_database) {
        cerr << "error: " << dlerror() << endl;
        return nullptr;
    }
    Database* db = make_database(folderTable);

    if (dlclose(handle)) {
        cerr << "error: " << dlerror() << endl;
        return nullptr;
    }

    return db;
}


void DatabaseTools::loadAndRunQuery(string filename, Database* db) {
    void* handle = dlopen((folderTmp + filename).c_str(), RTLD_NOW);
    if (!handle) {
        cerr << "error loading .so: " << dlerror() << endl;
        return;
    }

    auto query = reinterpret_cast<void (*)(Database*)>(dlsym(handle, "query"));
    if (!query) {
        cerr << "error: " << dlerror() << endl;
        return;
    }
    query(db);

    if (dlclose(handle)) {
        cerr << "error: " << dlerror() << endl;
        return;
    }
}

Schema* DatabaseTools::parseAndWriteSchema(const string& schemaFile) {
    Schema* schema = nullptr;
    SchemaParser p(schemaFile);
    try {
        schema = p.parse();
        cout << "Loaded " << schema->relations.size() << " relations into our schema." << endl;

        //Write to file the database
        ofstream myfile;
        myfile.open(folderTmp + dbName);
        myfile << schema->generateDatabaseCode();
        myfile.close();

        compileFile(dbName, dbNameCompiled);
    } catch (ParserError& e) {
        cerr << e.what() << " on line " << e.where() << endl;
    } catch (char const* msg) {
        cerr << "Error: " << msg << endl;
        schema = nullptr; //Reset pointer so that further execution stops
    }

    return schema;
}

string DatabaseTools::parseAndWriteQuery(const string& query, Schema* s) {
    string filename = "query_" + md5(query);
    ifstream f(folderTmp + filename + ".so");
    if (f.good()) { // Only compile if not already on disk
        return filename;
    }

    SQLLexer lexer(query);
    SQLParser q(lexer);
    unique_ptr<Query> qu = q.parse(s);

    ofstream myfile;
    myfile.open(folderTmp + filename + ".cpp");
    myfile << "#include <string>" << endl;
    myfile << "#include <map>" << endl;
    myfile << "#include <iostream>" << endl;
    myfile << "#include <tuple>" << endl
           << "#include \"db.cpp\"" << endl
           << "#include \"../utils/Types.hpp\"" << endl
           << "#include <algorithm>" << endl
           << "#include <iomanip>" << endl;
    myfile << "using namespace std;" << endl;
    myfile << "/* ";
    myfile << qu.get()->toString();
    myfile << " */ " << endl;
    myfile << "extern \"C\" void query(Database* db) {" << endl;
    if (qu.get()->shouldExplain()) {
        //Replace special characters
        string code = qu.get()->generateQueryCode();
        boost::replace_all(code, "\"", "\\\"");
        boost::replace_all(code, "\n", "\\n");

        myfile << "cout << \"" << code << "\";" << endl << endl;
    } else {
        myfile << qu.get()->generateQueryCode() ;
    }
    myfile << "}";
    myfile.close();

    return filename;
}
