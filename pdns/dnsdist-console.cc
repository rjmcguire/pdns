#include "dnsdist.hh"
#include "sodcrypto.hh"

#if defined (__OpenBSD__)
#include <readline/readline.h>
#include <readline/history.h>
#else
#include <editline/readline.h>
#endif

#include <fstream>
#include "dolog.hh"
#include "ext/json11/json11.hpp"

vector<pair<struct timeval, string> > g_confDelta;

// MUST BE CALLED UNDER A LOCK - right now the LuaLock
void feedConfigDelta(const std::string& line)
{
  if(line.empty())
    return;
  struct timeval now;
  gettimeofday(&now, 0);
  g_confDelta.push_back({now,line});
}

void doClient(ComboAddress server, const std::string& command)
{
  if(g_verbose)
    cout<<"Connecting to "<<server.toStringWithPort()<<endl;
  int fd=socket(server.sin4.sin_family, SOCK_STREAM, 0);
  if (fd < 0) {
    cerr<<"Unable to connect to "<<server.toStringWithPort()<<endl;
    return;
  }
  SConnect(fd, server);
  setTCPNoDelay(fd);
  SodiumNonce theirs, ours;
  ours.init();

  writen2(fd, (const char*)ours.value, sizeof(ours.value));
  readn2(fd, (char*)theirs.value, sizeof(theirs.value));

  if(!command.empty()) {
    string msg=sodEncryptSym(command, g_key, ours);
    putMsgLen32(fd, (uint32_t) msg.length());
    if(!msg.empty())
      writen2(fd, msg);
    uint32_t len;
    if(getMsgLen32(fd, &len)) {
      if (len > 0) {
        boost::scoped_array<char> resp(new char[len]);
        readn2(fd, resp.get(), len);
        msg.assign(resp.get(), len);
        msg=sodDecryptSym(msg, g_key, theirs);
        cout<<msg;
      }
    }
    else {
      cout << "Connection closed by the server." << endl;
    }
    close(fd);
    return; 
  }

  set<string> dupper;
  {
    ifstream history(".dnsdist_history");
    string line;
    while(getline(history, line))
      add_history(line.c_str());
  }
  ofstream history(".dnsdist_history", std::ios_base::app);
  string lastline;
  for(;;) {
    char* sline = readline("> ");
    rl_bind_key('\t',rl_complete);
    if(!sline)
      break;

    string line(sline);
    if(!line.empty() && line != lastline) {
      add_history(sline);
      history << sline <<endl;
      history.flush();
    }
    lastline=line;
    free(sline);
    
    if(line=="quit")
      break;

    /* no need to send an empty line to the server */
    if(line.empty())
      continue;

    string msg=sodEncryptSym(line, g_key, ours);
    putMsgLen32(fd, (uint32_t) msg.length());
    writen2(fd, msg);
    uint32_t len;
    if(!getMsgLen32(fd, &len)) {
      cout << "Connection closed by the server." << endl;
      break;
    }

    if (len > 0) {
      boost::scoped_array<char> resp(new char[len]);
      readn2(fd, resp.get(), len);
      msg.assign(resp.get(), len);
      msg=sodDecryptSym(msg, g_key, theirs);
      cout<<msg;
      cout.flush();
    }
    else {
      cout<<endl;
    }
  }
  close(fd);
}

void doConsole()
{
  set<string> dupper;
  {
    ifstream history(".dnsdist_history");
    string line;
    while(getline(history, line))
      add_history(line.c_str());
  }
  ofstream history(".dnsdist_history", std::ios_base::app);
  string lastline;
  for(;;) {
    char* sline = readline("> ");
    rl_bind_key('\t',rl_complete);
    if(!sline)
      break;

    string line(sline);
    if(!line.empty() && line != lastline) {
      add_history(sline);
      history << sline <<endl;
      history.flush();
    }
    lastline=line;
    free(sline);
    
    if(line=="quit")
      break;

    string response;
    try {
      bool withReturn=true;
    retry:;
      try {
        std::lock_guard<std::mutex> lock(g_luamutex);
        g_outputBuffer.clear();
        resetLuaSideEffect();
        auto ret=g_lua.executeCode<
          boost::optional<
            boost::variant<
              string, 
              shared_ptr<DownstreamState>,
              std::unordered_map<string, double>
              >
            >
          >(withReturn ? ("return "+line) : line);
        
        if(ret) {
          if (const auto strValue = boost::get<shared_ptr<DownstreamState>>(&*ret)) {
            cout<<(*strValue)->getName()<<endl;
          }
          else if (const auto strValue = boost::get<string>(&*ret)) {
            cout<<*strValue<<endl;
          }
          else if(const auto um = boost::get<std::unordered_map<string, double> >(&*ret)) {
            using namespace json11;
            Json::object o;
            for(const auto& v : *um)
              o[v.first]=v.second;
            Json out = o;
            cout<<out.dump()<<endl;
          }
        }
        else 
          cout << g_outputBuffer;
        if(!getLuaNoSideEffect())
          feedConfigDelta(line);
      }
      catch(const LuaContext::SyntaxErrorException&) {
        if(withReturn) {
          withReturn=false;
          goto retry;
        }
        throw;
      }
    }
    catch(const LuaContext::WrongTypeException& e) {
      std::cerr<<"Command returned an object we can't print"<<std::endl;
      // tried to return something we don't understand
    }
    catch(const LuaContext::ExecutionErrorException& e) {
      std::cerr << e.what(); 
      try {
        std::rethrow_if_nested(e);
        std::cerr << std::endl;
      } catch(const std::exception& e) {
        // e is the exception that was thrown from inside the lambda
        std::cerr << ": " << e.what() << std::endl;      
      }
      catch(const PDNSException& e) {
        // e is the exception that was thrown from inside the lambda
        std::cerr << ": " << e.reason << std::endl;      
      }
    }
    catch(const std::exception& e) {
      // e is the exception that was thrown from inside the lambda
      std::cerr << e.what() << std::endl;      
    }
  }
}
/**** CARGO CULT CODE AHEAD ****/
const std::vector<ConsoleKeyword> g_consoleKeywords{
  /* keyword, function, parameters, description */
  { "addACL", true, "netmask", "add to the ACL set who can use this server" },
  { "addAction", true, "DNS rule, DNS action", "add a rule" },
  { "addAnyTCRule", true, "", "generate TC=1 answers to ANY queries received over UDP, moving them to TCP" },
  { "addDelay", true, "domain, n", "delay answers within that domain by n milliseconds" },
  { "addDisableValidationRule", true, "DNS rule", "set the CD flags to 1 for all queries matching the specified domain" },
  { "addDNSCryptBind", true, "\"127.0.0.1:8443\", \"provider name\", \"/path/to/resolver.cert\", \"/path/to/resolver.key\", [false], [TCP Fast Open queue size]", "listen to incoming DNSCrypt queries on 127.0.0.1 port 8443, with a provider name of `provider name`, using a resolver certificate and associated key stored respectively in the `resolver.cert` and `resolver.key` files. The fifth optional parameter sets SO_REUSEPORT when available. The last parameter sets the TCP Fast Open queue size, enabling TCP Fast Open when available and the value is larger than 0" },
  { "addDomainBlock", true, "domain", "block queries within this domain" },
  { "addDomainSpoof", true, "domain, ip[, ip6]", "generate answers for A/AAAA/ANY queries using the ip parameters" },
  { "addDynBlocks", true, "addresses, message[, seconds]", "block the set of addresses with message `msg`, for `seconds` seconds (10 by default)" },
  { "addLocal", true, "netmask, [true], [false], [TCP Fast Open queue size]", "add to addresses we listen on. Second optional parameter sets TCP or not. Third optional parameter sets SO_REUSEPORT when available. Last parameter sets the TCP Fast Open queue size, enabling TCP Fast Open when available and the value is larger than 0" },
  { "addLuaAction", true, "x, func", "where 'x' is all the combinations from `addPoolRule`, and func is a function with the parameter `dq`, which returns an action to be taken on this packet. Good for rare packets but where you want to do a lot of processing" },
  { "addNoRecurseRule", true, "domain", "clear the RD flag for all queries matching the specified domain" },
  { "addPoolRule", true, "domain, pool", "send queries to this domain to that pool" },
  { "addQPSLimit", true, "domain, n", "limit queries within that domain to n per second" },
  { "addQPSPoolRule", true, "x, limit, pool", "like `addPoolRule`, but only select at most 'limit' queries/s for this pool, letting the subsequent rules apply otherwise" },
  { "addResponseAction", true, "DNS rule, DNS response action", "add a response rule" },
  { "AllowAction", true, "", "let these packets go through" },
  { "AllRule", true, "", "matches all traffic" },
  { "AndRule", true, "list of DNS rules", "matches if all sub-rules matches" },
  { "benchRule", true, "DNS Rule [, iterations [, suffix]]", "bench the specified DNS rule" },
  { "carbonServer", true, "serverIP, [ourname], [interval]", "report statistics to serverIP using our hostname, or 'ourname' if provided, every 'interval' seconds" },
  { "controlSocket", true, "addr", "open a control socket on this address / connect to this address in client mode" },
  { "clearDynBlocks", true, "", "clear all dynamic blocks" },
  { "clearRules", true, "", "remove all current rules" },
  { "DelayAction", true, "milliseconds", "delay the response by the specified amount of milliseconds (UDP-only)" },
  { "delta", true, "", "shows all commands entered that changed the configuration" },
  { "DisableValidationAction", true, "", "set the CD bit in the question, let it go through" },
  { "DropAction", true, "", "drop these packets" },
  { "dumpStats", true, "", "print all statistics we gather" },
  { "exceedNXDOMAINs", true, "rate, seconds", "get set of addresses that exceed `rate` NXDOMAIN/s over `seconds` seconds" },
  { "exceedQRate", true, "rate, seconds", "get set of address that exceed `rate` queries/s over `seconds` seconds" },
  { "exceedQTypeRate", true, "type, rate, seconds", "get set of address that exceed `rate` queries/s for queries of type `type` over `seconds` seconds" },
  { "exceedRespByterate", true, "rate, seconds", "get set of addresses that exeeded `rate` bytes/s answers over `seconds` seconds" },
  { "exceedServFails", true, "rate, seconds", "get set of addresses that exceed `rate` servails/s over `seconds` seconds" },
  { "firstAvailable", false, "", "picks the server with the lowest `order` that has not exceeded its QPS limit" },
  { "fixupCase", true, "bool", "if set (default to no), rewrite the first qname of the question part of the answer to match the one from the query. It is only useful when you have a downstream server that messes up the case of the question qname in the answer" },
  { "generateDNSCryptCertificate", true, "\"/path/to/providerPrivate.key\", \"/path/to/resolver.cert\", \"/path/to/resolver.key\", serial, validFrom, validUntil", "generate a new resolver private key and related certificate, valid from the `validFrom` timestamp until the `validUntil` one, signed with the provider private key" },
  { "generateDNSCryptProviderKeys", true, "\"/path/to/providerPublic.key\", \"/path/to/providerPrivate.key\"", "generate a new provider keypair"},
  { "getPoolServers", true, "pool", "return servers part of this pool" },
  { "getResponseRing", true, "", "return the current content of the response ring" },
  { "getServer", true, "n", "returns server with index n" },
  { "getServers", true, "", "returns a table with all defined servers" },
  { "grepq", true, "Netmask|DNS Name|100ms|{\"::1\", \"powerdns.com\", \"100ms\"} [, n]", "shows the last n queries and responses matching the specified client address or range (Netmask), or the specified DNS Name, or slower than 100ms" },
  { "leastOutstanding", false, "", "Send traffic to downstream server with least outstanding queries, with the lowest 'order', and within that the lowest recent latency"},
  { "LogAction", true, "[filename], [binary]", "Log a line for each query, to the specified file if any, to the console (require verbose) otherwise. When logging to a file, the `binary` optional parameter specifies whether we log in binary form (default) or in textual form" },
  { "makeKey", true, "", "generate a new server access key, emit configuration line ready for pasting" },
  { "MaxQPSIPRule", true, "qps, v4Mask=32, v6Mask=64", "matches traffic exceeding the qps limit per subnet" },
  { "MaxQPSRule", true, "qps", "matches traffic **not** exceeding this qps limit" },
  { "mvResponseRule", true, "from, to", "move response rule 'from' to a position where it is in front of 'to'. 'to' can be one larger than the largest rule" },
  { "mvRule", true, "from, to", "move rule 'from' to a position where it is in front of 'to'. 'to' can be one larger than the largest rule, in which case the rule will be moved to the last position" },
  { "newDNSName", true, "name", "make a DNSName based on this .-terminated name" },
  { "newQPSLimiter", true, "rate, burst", "configure a QPS limiter with that rate and that burst capacity" },
  { "newRemoteLogger", true, "address:port [, timeout=2, maxQueuedEntries=100, reconnectWaitTime=1]", "create a Remote Logger object, to use with `RemoteLogAction()` and `RemoteLogResponseAction()`" },
  { "newRuleAction", true, "DNS rule, DNS action", "return a pair of DNS Rule and DNS Action, to be used with `setRules()`" },
  { "newServer", true, "{address=\"ip:port\", qps=1000, order=1, weight=10, pool=\"abuse\", retries=5, tcpSendTimeout=30, tcpRecvTimeout=30, checkName=\"a.root-servers.net.\", checkType=\"A\", maxCheckFailures=1, mustResolve=false, useClientSubnet=true, source=\"address|interface name|address@interface\"", "instantiate a server" },
  { "newServerPolicy", true, "name, function", "create a policy object from a Lua function" },
  { "newSuffixMatchNode", true, "", "returns a new SuffixMatchNode" },
  { "NoRecurseAction", true, "", "strip RD bit from the question, let it go through" },
  { "PoolAction", true, "poolname", "set the packet into the specified pool" },
  { "printDNSCryptProviderFingerprint", true, "\"/path/to/providerPublic.key\"", "display the fingerprint of the provided resolver public key" },
  { "RegexRule", true, "regex", "matches the query name against the supplied regex" },
  { "RemoteLogAction", true, "RemoteLogger", "send the content of this query to a remote logger via Protocol Buffer" },
  { "RemoteLogResponseAction", true, "RemoteLogger", "send the content of this response to a remote logger via Protocol Buffer" },
  { "rmResponseRule", true, "n", "remove response rule n" },
  { "rmRule", true, "n", "remove rule n" },
  { "rmServer", true, "n", "remove server with index n" },
  { "roundrobin", false, "", "Simple round robin over available servers" },
  { "QNameLabelsCountRule", true, "min, max", "matches if the qname has less than `min` or more than `max` labels" },
  { "QNameWireLengthRule", true, "min, max", "matches if the qname's length on the wire is less than `min` or more than `max` bytes" },
  { "QTypeRule", true, "qtype", "matches queries with the specified qtype" },
  { "setACL", true, "{netmask, netmask}", "replace the ACL set with these netmasks. Use `setACL({})` to reset the list, meaning no one can use us" },
  { "setDNSSECPool", true, "pool name", "move queries requesting DNSSEC processing to this pool" },
  { "setECSOverride", true, "bool", "whether to override an existing EDNS Client Subnet value in the query" },
  { "setECSSourcePrefixV4", true, "prefix-length", "the EDNS Client Subnet prefix-length used for IPv4 queries" },
  { "setECSSourcePrefixV6", true, "prefix-length", "the EDNS Client Subnet prefix-length used for IPv6 queries" },
  { "setKey", true, "key", "set access key to that key" },
  { "setLocal", true, "netmask, [true], [false], [TCP Fast Open queue size]", "reset list of addresses we listen on to this address. Second optional parameter sets TCP or not. Third optional parameter sets SO_REUSEPORT when available. Last parameter sets the TCP Fast Open queue size, enabling TCP Fast Open when available and the value is larger than 0." },
  { "setMaxTCPClientThreads", true, "n", "set the maximum of TCP client threads, handling TCP connections" },
  { "setMaxTCPQueuedConnections", true, "n", "set the maximum number of TCP connections queued (waiting to be picked up by a client thread)" },
  { "setMaxUDPOutstanding", true, "n", "set the maximum number of outstanding UDP queries to a given backend server. This can only be set at configuration time and defaults to 10240" },
  { "setRules", true, "list of rules", "replace the current rules with the supplied list of pairs of DNS Rules and DNS Actions (see `newRuleAction()`)" },
  { "setServerPolicy", true, "policy", "set server selection policy to that policy" },
  { "setServerPolicyLua", true, "name, function", "set server selection policy to one named 'name' and provided by 'function'" },
  { "setTCPRecvTimeout", true, "n", "set the read timeout on TCP connections from the client, in seconds" },
  { "setTCPSendTimeout", true, "n", "set the write timeout on TCP connections from the client, in seconds" },
  { "setVerboseHealthChecks", true, "bool", "set whether health check errors will be logged" },
  { "show", true, "string", "outputs `string`" },
  { "showACL", true, "", "show our ACL set" },
  { "showDNSCryptBinds", true, "", "display the currently configured DNSCrypt binds" },
  { "showDynBlocks", true, "", "show dynamic blocks in force" },
  { "showResponseLatency", true, "", "show a plot of the response time latency distribution" },
  { "showResponseRules", true, "", "show all defined response rules" },
  { "showRules", true, "", "show all defined rules" },
  { "showServerPolicy", true, "", "show name of currently operational server selection policy" },
  { "showServers", true, "", "output all servers" },
  { "showTCPStats", true, "", "show some statistics regarding TCP" },
  { "showVersion", true, "", "show the current version" },
  { "shutdown", true, "", "shut down `dnsdist`" },
  { "SpoofAction", true, "{ip, ...} ", "forge a response with the specified IPv4 (for an A query) or IPv6 (for an AAAA). If you specify multiple addresses, all that match the query type (A, AAAA or ANY) will get spoofed in" },
  { "TCAction", true, "", "create answer to query with TC and RD bits set, to move to TCP" },
  { "testCrypto", true, "", "test of the crypto all works" },
  { "topBandwidth", true, "top", "show top-`top` clients that consume the most bandwidth over length of ringbuffer" },
  { "topClients", true, "n", "show top-`n` clients sending the most queries over length of ringbuffer" },
  { "topQueries", true, "n[, labels]", "show top 'n' queries, as grouped when optionally cut down to 'labels' labels" },
  { "topResponses", true, "n, kind[, labels]", "show top 'n' responses with RCODE=kind (0=NO Error, 2=ServFail, 3=ServFail), as grouped when optionally cut down to 'labels' labels" },
  { "topResponseRule", true, "", "move the last response rule to the first position" },
  { "topRule", true, "", "move the last rule to the first position" },
  { "topSlow", true, "[top][, limit][, labels]", "show `top` queries slower than `limit` milliseconds, grouped by last `labels` labels" },
  { "truncateTC", true, "bool", "if set (default) truncate TC=1 answers so they are actually empty. Fixes an issue for PowerDNS Authoritative Server 2.9.22" },
  { "webserver", true, "address:port, password [, apiKey [, customHeaders ]])", "launch a webserver with stats on that address with that password" },
  { "whashed", false, "", "Weighted hashed ('sticky') distribution over available servers, based on the server 'weight' parameter" },
  { "wrandom", false, "", "Weighted random over available servers, based on the server 'weight' parameter" },
};

extern "C" {
char* my_generator(const char* text, int state)
{
  string t(text);
  /* to keep it readable, we try to keep only 4 keywords per line
     and to start a new line when the first letter changes */
  static int s_counter=0;
  int counter=0;
  if(!state)
    s_counter=0;

  for(const auto& keyword : g_consoleKeywords) {
    if(boost::starts_with(keyword.name, t) && counter++ == s_counter)  {
      std::string value(keyword.name);
      s_counter++;
      if (keyword.function) {
        value += "(";
        if (keyword.parameters.empty()) {
          value += ")";
        }
      }
      return strdup(value.c_str());
    }
  }
  return 0;
}

char** my_completion( const char * text , int start,  int end)
{
  char **matches=0;
  if (start == 0)
    matches = rl_completion_matches ((char*)text, &my_generator);

  // skip default filename completion.
  rl_attempted_completion_over = 1;

  return matches;
}
}

void controlClientThread(int fd, ComboAddress client)
try
{
  setTCPNoDelay(fd);
  SodiumNonce theirs;
  readn2(fd, (char*)theirs.value, sizeof(theirs.value));
  SodiumNonce ours;
  ours.init();
  writen2(fd, (char*)ours.value, sizeof(ours.value));

  for(;;) {
    uint32_t len;
    if(!getMsgLen32(fd, &len))
      break;

    if (len == 0) {
      /* just ACK an empty message
         with an empty response */
      putMsgLen32(fd, 0);
      continue;
    }

    boost::scoped_array<char> msg(new char[len]);
    readn2(fd, msg.get(), len);
    
    string line(msg.get(), len);
    line = sodDecryptSym(line, g_key, theirs);
    //    cerr<<"Have decrypted line: "<<line<<endl;
    string response;
    try {
      bool withReturn=true;
    retry:;
      try {
        std::lock_guard<std::mutex> lock(g_luamutex);
        
        g_outputBuffer.clear();
        resetLuaSideEffect();
        auto ret=g_lua.executeCode<
          boost::optional<
            boost::variant<
              string, 
              shared_ptr<DownstreamState>,
              std::unordered_map<string, double>
              >
            >
          >(withReturn ? ("return "+line) : line);

      if(ret) {
	if (const auto strValue = boost::get<shared_ptr<DownstreamState>>(&*ret)) {
	  response=(*strValue)->getName()+"\n";
	}
	else if (const auto strValue = boost::get<string>(&*ret)) {
	  response=*strValue+"\n";
	}
        else if(const auto um = boost::get<std::unordered_map<string, double> >(&*ret)) {
          using namespace json11;
          Json::object o;
          for(const auto& v : *um)
            o[v.first]=v.second;
          Json out = o;
          response=out.dump()+"\n";
        }
      }
      else
	response=g_outputBuffer;
      if(!getLuaNoSideEffect())
        feedConfigDelta(line);
      }
      catch(const LuaContext::SyntaxErrorException&) {
        if(withReturn) {
          withReturn=false;
          goto retry;
        }
        throw;
      }
    }
    catch(const LuaContext::WrongTypeException& e) {
      response = "Command returned an object we can't print: " +std::string(e.what()) + "\n";
      // tried to return something we don't understand
    }
    catch(const LuaContext::ExecutionErrorException& e) {
      response = "Error: " + string(e.what()) + ": ";
      try {
        std::rethrow_if_nested(e);
      } catch(const std::exception& e) {
        // e is the exception that was thrown from inside the lambda
        response+= string(e.what());
      }
      catch(const PDNSException& e) {
        // e is the exception that was thrown from inside the lambda
        response += string(e.reason);
      }
    }
    catch(const LuaContext::SyntaxErrorException& e) {
      response = "Error: " + string(e.what()) + ": ";
    }
    response = sodEncryptSym(response, g_key, ours);
    putMsgLen32(fd, response.length());
    writen2(fd, response.c_str(), response.length());
  }
  infolog("Closed control connection from %s", client.toStringWithPort());
  close(fd);
  fd=-1;
}
catch(std::exception& e)
{
  errlog("Got an exception in client connection from %s: %s", client.toStringWithPort(), e.what());
  if(fd >= 0)
    close(fd);
}

