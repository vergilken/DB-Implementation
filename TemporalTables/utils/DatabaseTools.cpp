
#include "DatabaseTools.h"


using queryType = void (*)(Database*, vector<string>, bool);
const string DatabaseTools::dbName = "db";
const string DatabaseTools::folderTmp = "tmp/";
const string DatabaseTools::folderTable = "./tblTemporal/";
//Debug symbols: -g  -O0 -DDEBUG -ggdb3 / Additional: -flto  -pipe
const char* DatabaseTools::cmdBuild{"g++ -O3 -std=c++14 -fPIC -flto -pipe %s -shared -o %s\0"};


void DatabaseTools::split(const std::string& str, std::vector<std::string>& lineChunks) {
    std::stringstream in(str);
    std::string segment;
    lineChunks.clear();
    while (std::getline(in, segment, '|')) {
        lineChunks.push_back(segment);
    }
}

long DatabaseTools::compileFile(const string name) {
    using namespace std::chrono;

    //Start the clock
    high_resolution_clock::time_point start = high_resolution_clock::now();

    //Assemble file names
    string fileIn = folderTmp + name + ".cpp";
    string fileOut = folderTmp + name + ".so";

    //Cache queries on disk
    ifstream f(fileOut);
    if (f.good() && name != dbName) { // Only compile if not already on disk
        return duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
    }

    //Build the command by replacing the anchors / placeholders with the correct values
    char* command = new char[strlen(cmdBuild) + fileIn.size() + fileOut.size() + 5];
    sprintf(command, cmdBuild, fileIn.c_str(), fileOut.c_str());
    int ret = system(command);
    delete[] command;

    //If compilation failed
    if (ret != 0) {
        return -1;
    }

    //Stop an return the execution time
    return duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
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


long DatabaseTools::loadAndRunQuery(string filename, Database* db, vector<string>& tmp) {
    using namespace std::chrono;

    string filenameExt = folderTmp + filename + ".so";

    //Start the clock
    high_resolution_clock::time_point start = high_resolution_clock::now();

    //Load up the dynamic lib
    void* handle = dlopen(filenameExt.c_str(), RTLD_NOW);
    if (!handle) {
        cerr << "error loading " << filenameExt << ": " << dlerror() << endl;
        return 0;
    }

    //Get the function pointer
    auto query = reinterpret_cast<queryType>(dlsym(handle, "query"));
    if (!query) {
        cerr << "error: " << dlerror() << endl;
        return 0;
    }

    //Execute
    query(db, tmp, true);

    //Unload
    if (dlclose(handle)) {
        cerr << "error: " << dlerror() << endl;
        return 0;
    }

    //Stop an return the execution time
    return duration_cast<microseconds>(high_resolution_clock::now() - start).count();
}

Schema* DatabaseTools::parseAndWriteSchema(const string& schemaFile) {
    Schema* schema = nullptr;
    SchemaParser p(schemaFile);
    try {
        schema = p.parse();
        cout << "Loaded " << schema->relations.size() << " relations into our schema." << endl;

        //Write to file the database
        ofstream myfile;
        myfile.open(folderTmp + dbName + ".cpp");
        myfile << schema->generateDatabaseCode();
        myfile.close();

        compileFile(dbName);
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
    Query* qu = q.parse(s);

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
    myfile << qu->toString();
    myfile << " */ " << endl;
    myfile << "extern \"C\" void query(Database* db, const vector<string>& params, bool output) {" << endl;
    if (qu->shouldExplain()) {
        //Replace special characters
        string code = qu->generateQueryCode();
        boost::replace_all(code, "\"", "\\\"");
        boost::replace_all(code, "\n", "\\n");

        myfile << "cout << \"";
        myfile << code << "\";" << endl << endl;
    } else {
        myfile << qu->generateQueryCode();
    }
    myfile << "}";
    myfile.close();

    return filename;
}

static const char charset[] =
        "0123456789"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz";
static auto randChar = []() -> char {
    const size_t max_index = (sizeof(charset) - 1);
    return charset[rand() % max_index];
};

void genRandom(string& str, size_t length) {
    generate_n(str.begin(), length, randChar);
}

void DatabaseTools::performanceTest(Schema* s, Database* db) {
    using namespace std::chrono;

    int iterationsInsert = 4000000;
    int iterationsUpdate = 4000000;
    long time = 0, timeTemporal;
    vector<string> params{"", ""};

    cout << "Testing performance - this may take some time (" << iterationsInsert << " iterations)" << endl;
    cout << "compiling queries first";
    //Load all queries
    string queriesTemporal[3], queriesNormal[3];

    {
#pragma omp parallel sections
        {
#pragma omp section
            {
                queriesTemporal[0] = DatabaseTools::parseAndWriteQuery("INSERT INTO warehouse (w_id, w_city) VALUES (?,?)", s);
                DatabaseTools::compileFile(queriesTemporal[0]);
            }
#pragma omp section
            {
                queriesTemporal[1] = DatabaseTools::parseAndWriteQuery("UPDATE warehouse SET w_city=? WHERE w_id=?", s);
                DatabaseTools::compileFile(queriesTemporal[1]);
            }
#pragma omp section
            {
                queriesTemporal[2] = DatabaseTools::parseAndWriteQuery("DELETE FROM warehouse WHERE w_id=?", s);
                DatabaseTools::compileFile(queriesTemporal[2]);
            }
#pragma omp section
            {
                queriesNormal[0] = DatabaseTools::parseAndWriteQuery("INSERT INTO warehouseold (w_id, w_city) VALUES (?,?)", s);
                DatabaseTools::compileFile(queriesNormal[0]);
            }
#pragma omp section
            {
                queriesNormal[1] = DatabaseTools::parseAndWriteQuery("UPDATE warehouseold SET w_city=? WHERE w_id=?", s);
                DatabaseTools::compileFile(queriesNormal[1]);
            }

#pragma omp section
            {
                queriesNormal[2] = DatabaseTools::parseAndWriteQuery("DELETE FROM warehouseold WHERE w_id=?", s);
                DatabaseTools::compileFile(queriesNormal[2]);
            }
        }
    }
    cout << ": done." << endl;

    {
        //Inserts Temporal
        {
            string filenameExt = folderTmp + queriesTemporal[0] + ".so";
            void* handle = dlopen(filenameExt.c_str(), RTLD_NOW);
            high_resolution_clock::time_point start = high_resolution_clock::now();
            auto query = reinterpret_cast<queryType>(dlsym(handle, "query"));

            for (int i = 6; i < iterationsInsert; i++) {
                params[0] = to_string(i);
                genRandom(params[1], 10);
                query(db, params, false);
            }

            dlclose(handle);
            timeTemporal = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
            cout << "Insert - Temporal: " << timeTemporal << "ms (" << (iterationsInsert / (timeTemporal / 1000.0)) / 1000.0 << " kO/s )";
        }

        //Inserts Normal
        {
            string filenameExt = folderTmp + queriesNormal[0] + ".so";
            void* handle = dlopen(filenameExt.c_str(), RTLD_NOW);
            high_resolution_clock::time_point start = high_resolution_clock::now();
            auto query = reinterpret_cast<queryType>(dlsym(handle, "query"));

            for (int i = 6; i < iterationsInsert; i++) {
                params[0] = to_string(i);
                genRandom(params[1], 10);
                query(db, params, false);
            }

            dlclose(handle);
            time = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
            cout << " / Normal: " << time << "ms (" << (iterationsInsert / (time / 1000.0)) / 1000.0 << " kO/s )";
        }
        cout << " / " << (100.0 - (double) time / (double) timeTemporal * 100.0) << "% slower" << endl;
    }

    //Updates
    try {
        //Temporal
        {
            string filenameExt = folderTmp + queriesTemporal[1] + ".so";
            void* handle = dlopen(filenameExt.c_str(), RTLD_NOW);
            high_resolution_clock::time_point start = high_resolution_clock::now();
            auto query = reinterpret_cast<queryType>(dlsym(handle, "query"));

            for (int i = 6; i < iterationsUpdate; i++) {
                genRandom(params[0], 10);
                params[1] = to_string(i);
                query(db, params, false);
            }

            dlclose(handle);
            timeTemporal = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
            cout << "Update - Temporal: " << timeTemporal << "ms (" << (iterationsUpdate / (timeTemporal / 1000.0)) / 1000.0 << " kO/s )";
        }

        //Normal
        {
            string filenameExt = folderTmp + queriesNormal[1] + ".so";
            void* handle = dlopen(filenameExt.c_str(), RTLD_NOW);
            high_resolution_clock::time_point start = high_resolution_clock::now();
            auto query = reinterpret_cast<queryType>(dlsym(handle, "query"));

            for (int i = 6; i < iterationsUpdate; i++) {
                genRandom(params[0], 10);
                params[1] = to_string(i);
                query(db, params, false);
            }

            dlclose(handle);
            time = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
            cout << " / Normal: " << time << "ms (" << (iterationsUpdate / (time / 1000.0)) / 1000.0 << " kO/s )";
        }
        cout << " / " << (100.0 - (double) time / (double) timeTemporal * 100.0) << "% slower" << endl;
    } catch (const char* e) {
        cout << e << endl;
    }

    //Delete
    try {
        //Temporal
        {
            string filenameExt = folderTmp + queriesTemporal[2] + ".so";
            void* handle = dlopen(filenameExt.c_str(), RTLD_NOW);
            high_resolution_clock::time_point start = high_resolution_clock::now();
            auto query = reinterpret_cast<queryType>(dlsym(handle, "query"));

            for (int i = 0; i < iterationsInsert; i++) {
                params[0] = to_string(i);
                query(db, params, false);
            }

            dlclose(handle);
            timeTemporal = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
            cout << "Delete - Temporal: " << timeTemporal << "ms (" << (iterationsInsert / (timeTemporal / 1000.0)) / 1000.0 << " kO/s )";
        }

        //Normal
        {
            string filenameExt = folderTmp + queriesNormal[2] + ".so";
            void* handle = dlopen(filenameExt.c_str(), RTLD_NOW);
            high_resolution_clock::time_point start = high_resolution_clock::now();
            auto query = reinterpret_cast<queryType>(dlsym(handle, "query"));

            for (int i = 0; i < iterationsInsert; i++) {
                params[0] = to_string(i);
                query(db, params, false);
            }

            dlclose(handle);
            time = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
            cout << " / Normal: " << time << "ms (" << (iterationsInsert / (time / 1000.0)) / 1000.0 << " kO/s )";
        }
        cout << " / " << -(100.0 - (double) time / (double) timeTemporal * 100.0) << "% faster" << endl;
    } catch (const char* e) {
        cout << e << endl;
    }

    //Output the final sizes
    void* handle = dlopen((folderTmp + dbName + ".so").c_str(), RTLD_NOW);
    auto getSize = reinterpret_cast<size_t (*)(Database*, string)>(dlsym(handle, "getSize"));
    cout << "Tables size - temporal: " << getSize(db, "wh") << " / normal: " << getSize(db, "who") << endl;
}

void DatabaseTools::performanceTest2(Schema* s, Database* db) {
    using namespace std::chrono;

    int iterationsInsert = 50000;
    int iterationsSelect = 500;
    int iterationsRounds = 5;
    long time = 0, timeTemporal = 0;
    vector<string> params{"", ""};

    cout << "Testing performance w/ select - this may take some time" << endl;
    cout << "compiling queries first";
    //Load all queries
    string queriesTemporal[3], queriesNormal[3];

    {
#pragma omp parallel sections
        {
#pragma omp section
            {
                queriesTemporal[0] = DatabaseTools::parseAndWriteQuery("INSERT INTO warehouse (w_id, w_city) VALUES (?,?)", s);
                DatabaseTools::compileFile(queriesTemporal[0]);
            }
#pragma omp section
            {
                queriesTemporal[1] = DatabaseTools::parseAndWriteQuery("UPDATE warehouse SET w_city=? WHERE w_id=?", s);
                DatabaseTools::compileFile(queriesTemporal[1]);
            }
#pragma omp section
            {
                queriesTemporal[2] = DatabaseTools::parseAndWriteQuery("SELECT * FROM warehouse", s);
                DatabaseTools::compileFile(queriesTemporal[2]);
            }
#pragma omp section
            {
                queriesNormal[0] = DatabaseTools::parseAndWriteQuery("INSERT INTO warehouseold (w_id, w_city) VALUES (?,?)", s);
                DatabaseTools::compileFile(queriesNormal[0]);
            }
#pragma omp section
            {
                queriesNormal[1] = DatabaseTools::parseAndWriteQuery("UPDATE warehouseold SET w_city=? WHERE w_id=?", s);
                DatabaseTools::compileFile(queriesNormal[1]);
            }
#pragma omp section
            {
                queriesNormal[2] = DatabaseTools::parseAndWriteQuery("SELECT * FROM warehouseold", s);
                DatabaseTools::compileFile(queriesNormal[2]);
            }
        }
    }
    cout << ": done." << endl;

    //Need to check table sizes
    void* handleGetSize = dlopen((folderTmp + dbName + ".so").c_str(), RTLD_NOW);
    auto getSize = reinterpret_cast<size_t (*)(Database*, string)>(dlsym(handleGetSize, "getSize"));

    {

        string filenameExt1 = folderTmp + queriesTemporal[0] + ".so";
        void* handleInsert = dlopen(filenameExt1.c_str(), RTLD_NOW);
        string filenameExt2 = folderTmp + queriesTemporal[1] + ".so";
        void* handleUpdate = dlopen(filenameExt2.c_str(), RTLD_NOW);
        string filenameExt3 = folderTmp + queriesTemporal[2] + ".so";
        void* handleSelect = dlopen(filenameExt3.c_str(), RTLD_NOW);

        auto queryInsert = reinterpret_cast<queryType>(dlsym(handleInsert, "query"));
        auto queryUpdate = reinterpret_cast<queryType>(dlsym(handleUpdate, "query"));
        auto querySelect = reinterpret_cast<queryType>(dlsym(handleSelect, "query"));


        int pkAutoIncrement = 5;
        for (int i = 0; i < iterationsRounds; i++) {
            int target = pkAutoIncrement + iterationsInsert;

            //Do some inserts
            for (; pkAutoIncrement < target; pkAutoIncrement++) {
                params[0] = to_string(pkAutoIncrement);
                genRandom(params[1], 10);
                queryInsert(db, params, false);
            }

            //Do some updates
            pkAutoIncrement -= iterationsInsert;
            for (; pkAutoIncrement < target; pkAutoIncrement++) {
                params[1] = to_string(pkAutoIncrement);
                genRandom(params[0], 10);
                queryUpdate(db, params, false);
            }

            //Test table scan
            high_resolution_clock::time_point start = high_resolution_clock::now();
            for (int j = 0; j < iterationsSelect; j++) {
                querySelect(db, params, false);
            }
            time = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
            cout << "With " << getSize(db, "wh") << " records took: " << time << "ms" << endl;
        }

        dlclose(handleInsert);
        dlclose(handleUpdate);
        dlclose(handleSelect);
    }
    cout << endl;
    cout << "old fashion: " << endl;
    try {

        string filenameExt1 = folderTmp + queriesNormal[0] + ".so";
        void* handleInsert = dlopen(filenameExt1.c_str(), RTLD_NOW);
        string filenameExt2 = folderTmp + queriesNormal[1] + ".so";
        void* handleUpdate = dlopen(filenameExt2.c_str(), RTLD_NOW);
        string filenameExt3 = folderTmp + queriesNormal[2] + ".so";
        void* handleSelect = dlopen(filenameExt3.c_str(), RTLD_NOW);

        auto queryInsert = reinterpret_cast<queryType>(dlsym(handleInsert, "query"));
        auto queryUpdate = reinterpret_cast<queryType>(dlsym(handleUpdate, "query"));
        auto querySelect = reinterpret_cast<queryType>(dlsym(handleSelect, "query"));


        int pkAutoIncrement = 5;
        for (int i = 0; i < iterationsRounds; i++) {
            int target = pkAutoIncrement + iterationsInsert;

            //Do some inserts
            for (; pkAutoIncrement < target; pkAutoIncrement++) {
                params[0] = to_string(pkAutoIncrement);
                genRandom(params[1], 10);
                try {
                    queryInsert(db, params, false);
                } catch (const char* e) {}
            }

            //Do some updates
            pkAutoIncrement -= iterationsInsert;
            for (; pkAutoIncrement < target; pkAutoIncrement++) {
                params[1] = to_string(pkAutoIncrement);
                genRandom(params[0], 10);
                queryUpdate(db, params, false);
            }

            //Test table scan
            high_resolution_clock::time_point start = high_resolution_clock::now();
            for (int j = 0; j < iterationsSelect; j++) {
                querySelect(db, params, false);
            }
            time = duration_cast<milliseconds>(high_resolution_clock::now() - start).count();
            cout << "With " << getSize(db, "who") << " records took: " << time << "ms" << endl;
        }

        dlclose(handleInsert);
        dlclose(handleUpdate);
        dlclose(handleSelect);
    } catch (const char* e) {
        cerr << e << endl;
    }

    dlclose(handleGetSize);
}
