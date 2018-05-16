#include <crow/app.h>

#include <tinyxml2.h>
#include <dbus_singleton.hpp>
#include <experimental/filesystem>
#include <fstream>
#include <boost/algorithm/string.hpp>
#include <boost/container/flat_set.hpp>

namespace crow {
namespace openbmc_mapper {

void introspect_objects(crow::response &res, std::string process_name,
                        std::string path,
                        std::shared_ptr<nlohmann::json> transaction) {
  crow::connections::system_bus->async_method_call(
      [
        &res, transaction, process_name{std::move(process_name)},
        object_path{std::move(path)}
      ](const boost::system::error_code ec, const std::string &introspect_xml) {
        if (ec) {
          CROW_LOG_ERROR << "Introspect call failed with error: "
                         << ec.message() << " on process: " << process_name
                         << " path: " << object_path << "\n";

        } else {
          transaction->push_back({{"path", object_path}});

          tinyxml2::XMLDocument doc;

          doc.Parse(introspect_xml.c_str());
          tinyxml2::XMLNode *pRoot = doc.FirstChildElement("node");
          if (pRoot == nullptr) {
            CROW_LOG_ERROR << "XML document failed to parse " << process_name
                           << " " << object_path << "\n";

          } else {
            tinyxml2::XMLElement *node = pRoot->FirstChildElement("node");
            while (node != nullptr) {
              std::string child_path = node->Attribute("name");
              std::string newpath;
              if (object_path != "/") {
                newpath += object_path;
              }
              newpath += "/" + child_path;
              // introspect the subobjects as well
              introspect_objects(res, process_name, newpath, transaction);

              node = node->NextSiblingElement("node");
            }
          }
        }
        // if we're the last outstanding caller, finish the request
        if (transaction.use_count() == 1) {
          res.json_value = {{"status", "ok"},
                            {"bus_name", process_name},
                            {"objects", std::move(*transaction)}};
          res.end();
        }
      },
      process_name, path, "org.freedesktop.DBus.Introspectable", "Introspect");
}

// A smattering of common types to unpack.  TODO(ed) this should really iterate
// the sdbusplus object directly and build the json response
using DbusRestVariantType = sdbusplus::message::variant<
    std::vector<std::tuple<std::string, std::string, std::string>>, std::string,
    int64_t, uint64_t, double, int32_t, uint32_t, int16_t, uint16_t, uint8_t,
    bool>;

using ManagedObjectType = std::vector<std::pair<
    sdbusplus::message::object_path,
    boost::container::flat_map<
        std::string,
        boost::container::flat_map<std::string, DbusRestVariantType>>>>;

void get_managed_objects_for_enumerate(
    const std::string &object_name, const std::string &connection_name,
    crow::response &res, std::shared_ptr<nlohmann::json> transaction) {
  crow::connections::system_bus->async_method_call(
      [&res, transaction](const boost::system::error_code ec,
                          const ManagedObjectType &objects) {
        if (ec) {
          CROW_LOG_ERROR << ec;
        } else {
          nlohmann::json &data_json = *transaction;

          for (auto &object_path : objects) {
            CROW_LOG_DEBUG << "Reading object "
                           << static_cast<const std::string &>(
                                  object_path.first);
            nlohmann::json &object_json =
                data_json[static_cast<const std::string &>(object_path.first)];
            if (object_json.is_null()) {
              object_json = nlohmann::json::object();
            }
            for (const auto &interface : object_path.second) {
              for (const auto &property : interface.second) {
                nlohmann::json &property_json = object_json[property.first];
                mapbox::util::apply_visitor(
                    [&property_json](auto &&val) { property_json = val; },
                    property.second);

                // dbus-rest represents booleans as 1 or 0, implement to match
                // TODO(ed) see if dbus-rest should be changed
                const bool *property_bool =
                    property_json.get_ptr<const bool *>();
                if (property_bool != nullptr) {
                  property_json = *property_bool ? 1 : 0;
                }
              }
            }
          }
        }

        if (transaction.use_count() == 1) {
          res.json_value = {{"message", "200 OK"},
                            {"status", "ok"},
                            {"data", std::move(*transaction)}};
          res.end();
        }
      },
      connection_name, object_name, "org.freedesktop.DBus.ObjectManager",
      "GetManagedObjects");
}

using GetSubTreeType = std::vector<
    std::pair<std::string,
              std::vector<std::pair<std::string, std::vector<std::string>>>>>;

// Structure for storing data on an in progress action
struct InProgressActionData {
  InProgressActionData(crow::response &res) : res(res){};
  ~InProgressActionData() {
    if (res.result() == boost::beast::http::status::internal_server_error) {
      // Reset the json object to clear out any data that made it in before the
      // error happened
      // todo(ed) handle error condition with proper code
      res.json_value = nlohmann::json::object();
    }
    res.end();
  }

  void setErrorStatus() {
    res.result(boost::beast::http::status::internal_server_error);
  }
  crow::response &res;
  std::string path;
  std::string method_name;
  nlohmann::json arguments;
};

std::vector<std::string> dbus_arg_split(const std::string &string) {
  std::vector<std::string> ret;
  if (string.empty()) {
    return ret;
  }
  ret.push_back("");
  int container_depth = 0;
  std::string::const_iterator character = string.begin();
  while (character != string.end()) {
    switch (*character) {
      case ('a'):
        ret.back() += *character;
        break;
      case ('('):
      case ('{'):
        ret.back() += *character;
        container_depth++;
        break;
      case ('}'):
      case (')'):
        ret.back() += *character;
        container_depth--;
        if (container_depth == 0) {
          character++;
          if (character != string.end()) {
            ret.push_back("");
          }
          continue;
        }
        break;
      default:
        ret.back() += *character;
        if (container_depth == 0) {
          character++;
          if (character != string.end()) {
            ret.push_back("");
          }
          continue;
        }
        break;
    }
    character++;
  }
}

int convert_json_to_dbus(sd_bus_message *m, const std::string &arg_type,
                         const nlohmann::json &input_json) {
  int r = 0;
  CROW_LOG_DEBUG << "Converting " << input_json.dump()
                 << " to type: " << arg_type;
  const std::vector<std::string> arg_types = dbus_arg_split(arg_type);

  // Assume a single object for now.
  const nlohmann::json *j = &input_json;
  nlohmann::json::const_iterator j_it = input_json.begin();

  for (const std::string &arg_code : arg_types) {
    // If we are decoding multiple objects, grab the pointer to the iterator,
    // and increment it for the next loop
    if (arg_types.size() > 1) {
      if (j_it == input_json.end()) {
        return -2;
      }
      j = &*j_it;
      j_it++;
    }
    const int64_t *int_value = j->get_ptr<const int64_t *>();
    const uint64_t *uint_value = j->get_ptr<const uint64_t *>();
    const std::string *string_value = j->get_ptr<const std::string *>();
    const double *double_value = j->get_ptr<const double *>();
    const bool *b = j->get_ptr<const bool *>();
    int64_t v = 0;
    double d = 0.0;

    // Do some basic type conversions that make sense.  uint can be converted to
    // int.  int and uint can be converted to double
    if (uint_value != nullptr && int_value == nullptr) {
      v = static_cast<int64_t>(*uint_value);
      int_value = &v;
    }
    if (uint_value != nullptr && double_value == nullptr) {
      d = static_cast<double>(*uint_value);
      double_value = &d;
    }
    if (int_value != nullptr && double_value == nullptr) {
      d = static_cast<double>(*int_value);
      double_value = &d;
    }

    if (arg_code == "s") {
      if (string_value == nullptr) {
        return -1;
      }
      r = sd_bus_message_append_basic(m, arg_code[0],
                                      (void *)string_value->c_str());
      if (r < 0) {
        return r;
      }
    } else if (arg_code == "i") {
      if (int_value == nullptr) {
        return -1;
      }
      int32_t i = static_cast<int32_t>(*int_value);
      r = sd_bus_message_append_basic(m, arg_code[0], &i);
      if (r < 0) {
        return r;
      }
    } else if (arg_code == "b") {
      // lots of ways bool could be represented here.  Try them all
      int bool_int = false;
      if (int_value != nullptr) {
        bool_int = *int_value > 0 ? 1 : 0;
      } else if (b != nullptr) {
        bool_int = b ? 1 : 0;
      } else if (string_value != nullptr) {
        bool_int = boost::istarts_with(*string_value, "t") ? 1 : 0;
      } else {
        return -1;
      }
      r = sd_bus_message_append_basic(m, arg_code[0], &bool_int);
      if (r < 0) {
        return r;
      }
    } else if (arg_code == "n") {
      if (int_value == nullptr) {
        return -1;
      }
      int16_t n = static_cast<int16_t>(*int_value);
      r = sd_bus_message_append_basic(m, arg_code[0], &n);
      if (r < 0) {
        return r;
      }
    } else if (arg_code == "x") {
      if (int_value == nullptr) {
        return -1;
      }
      r = sd_bus_message_append_basic(m, arg_code[0], int_value);
      if (r < 0) {
        return r;
      }
    } else if (arg_code == "y") {
      if (uint_value == nullptr) {
        return -1;
      }
      uint8_t y = static_cast<uint8_t>(*uint_value);
      r = sd_bus_message_append_basic(m, arg_code[0], &y);
    } else if (arg_code == "q") {
      if (uint_value == nullptr) {
        return -1;
      }
      uint16_t q = static_cast<uint16_t>(*uint_value);
      r = sd_bus_message_append_basic(m, arg_code[0], &q);
    } else if (arg_code == "u") {
      if (uint_value == nullptr) {
        return -1;
      }
      uint32_t u = static_cast<uint32_t>(*uint_value);
      r = sd_bus_message_append_basic(m, arg_code[0], &u);
    } else if (arg_code == "t") {
      if (uint_value == nullptr) {
        return -1;
      }
      r = sd_bus_message_append_basic(m, arg_code[0], uint_value);
    } else if (arg_code == "d") {
      sd_bus_message_append_basic(m, arg_code[0], double_value);
    } else if (boost::starts_with(arg_code, "a")) {
      std::string contained_type = arg_code.substr(1);
      r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY,
                                        contained_type.c_str());
      if (r < 0) {
        return r;
      }

      for (nlohmann::json::const_iterator it = j->begin(); it != j->end();
           ++it) {
        r = convert_json_to_dbus(m, contained_type, *it);
        if (r < 0) {
          return r;
        }

        it++;
      }
      sd_bus_message_close_container(m);
    } else if (boost::starts_with(arg_code, "v")) {
      std::string contained_type = arg_code.substr(1);
      CROW_LOG_DEBUG << "variant type: " << arg_code
                     << " appending variant of type: " << contained_type;
      r = sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT,
                                        contained_type.c_str());
      if (r < 0) {
        return r;
      }

      r = convert_json_to_dbus(m, contained_type, input_json);
      if (r < 0) {
        return r;
      }

      r = sd_bus_message_close_container(m);
      if (r < 0) {
        return r;
      }

    } else if (boost::starts_with(arg_code, "(") &&
               boost::ends_with(arg_code, ")")) {
      std::string contained_type = arg_code.substr(1, arg_code.size() - 1);
      r = sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT,
                                        contained_type.c_str());
      nlohmann::json::const_iterator it = j->begin();
      for (const std::string &arg_code : dbus_arg_split(arg_type)) {
        if (it == j->end()) {
          return -1;
        }
        r = convert_json_to_dbus(m, arg_code, *it);
        if (r < 0) {
          return r;
        }
        it++;
      }
      r = sd_bus_message_close_container(m);
    } else if (boost::starts_with(arg_code, "{") &&
               boost::ends_with(arg_code, "}")) {
      std::string contained_type = arg_code.substr(1, arg_code.size() - 1);
      r = sd_bus_message_open_container(m, SD_BUS_TYPE_DICT_ENTRY,
                                        contained_type.c_str());
      std::vector<std::string> codes = dbus_arg_split(contained_type);
      if (codes.size() != 2) {
        return -1;
      }
      const std::string &key_type = codes[0];
      const std::string &value_type = codes[1];
      for (auto it : j->items()) {
        r = convert_json_to_dbus(m, key_type, it.key());
        if (r < 0) {
          return r;
        };

        r = convert_json_to_dbus(m, value_type, it.value());
        if (r < 0) {
          return r;
        }
      }
      r = sd_bus_message_close_container(m);
    } else {
      return -2;
    }
    if (r < 0) {
      return r;
    }

    if (arg_types.size() > 1) {
      j_it++;
    }
  }
}

void find_action_on_interface(std::shared_ptr<InProgressActionData> transaction,
                              const std::string &connectionName) {
  CROW_LOG_DEBUG << "find_action_on_interface for connection "
                 << connectionName;
  crow::connections::system_bus->async_method_call(
      [
        transaction, connectionName{std::string(connectionName)}
      ](const boost::system::error_code ec, const std::string &introspect_xml) {
        CROW_LOG_DEBUG << "got xml:\n " << introspect_xml;
        if (ec) {
          CROW_LOG_ERROR << "Introspect call failed with error: "
                         << ec.message() << " on process: " << connectionName
                         << "\n";
        } else {
          tinyxml2::XMLDocument doc;

          doc.Parse(introspect_xml.c_str());
          tinyxml2::XMLNode *pRoot = doc.FirstChildElement("node");
          if (pRoot == nullptr) {
            CROW_LOG_ERROR << "XML document failed to parse " << connectionName
                           << "\n";

          } else {
            tinyxml2::XMLElement *interface_node =
                pRoot->FirstChildElement("interface");
            while (interface_node != nullptr) {
              std::string this_interface_name =
                  interface_node->Attribute("name");
              tinyxml2::XMLElement *method_node =
                  interface_node->FirstChildElement("method");
              while (method_node != nullptr) {
                std::string this_method_name = method_node->Attribute("name");
                CROW_LOG_DEBUG << "Found method: " << this_method_name;
                if (this_method_name == transaction->method_name) {
                  sdbusplus::message::message m =
                      crow::connections::system_bus->new_method_call(
                          connectionName.c_str(), transaction->path.c_str(),
                          this_interface_name.c_str(),
                          transaction->method_name.c_str());

                  tinyxml2::XMLElement *argument_node =
                      method_node->FirstChildElement("arg");

                  nlohmann::json::const_iterator arg_it =
                      transaction->arguments.begin();

                  while (argument_node != nullptr) {
                    std::string arg_direction =
                        argument_node->Attribute("direction");
                    if (arg_direction == "in") {
                      std::string arg_type = argument_node->Attribute("type");
                      if (arg_it == transaction->arguments.end()) {
                        transaction->setErrorStatus();
                        return;
                      }
                      if (convert_json_to_dbus(m.get(), arg_type, *arg_it) <
                          0) {
                        transaction->setErrorStatus();
                        return;
                      }

                      arg_it++;
                    }
                    argument_node = method_node->NextSiblingElement("arg");
                  }
                  crow::connections::system_bus->async_send(
                      m, [transaction](boost::system::error_code ec,
                                       sdbusplus::message::message &m) {
                        if (ec) {
                          transaction->setErrorStatus();
                          return;
                        }
                        transaction->res.json_value = {{"status", "ok"},
                                                       {"message", "200 OK"},
                                                       {"data", nullptr}};
                      });
                  break;
                }
                method_node = method_node->NextSiblingElement("method");
              }
              interface_node = interface_node->NextSiblingElement("interface");
            }
          }
        }
      },
      connectionName, transaction->path, "org.freedesktop.DBus.Introspectable",
      "Introspect");
}

void handle_action(const crow::request &req, crow::response &res,
                   const std::string &object_path,
                   const std::string &method_name) {
  nlohmann::json request_dbus_data =
      nlohmann::json::parse(req.body, nullptr, false);

  if (request_dbus_data.is_discarded()) {
    res.result(boost::beast::http::status::bad_request);
    res.end();
    return;
  }
  if (!request_dbus_data.is_array()) {
    res.result(boost::beast::http::status::bad_request);
    res.end();
    return;
  }
  auto transaction = std::make_shared<InProgressActionData>(res);

  transaction->path = object_path;
  transaction->method_name = method_name;
  transaction->arguments = std::move(request_dbus_data);
  crow::connections::system_bus->async_method_call(
      [transaction](
          const boost::system::error_code ec,
          const std::vector<std::pair<std::string, std::vector<std::string>>>
              &interface_names) {
        if (ec || interface_names.size() <= 0) {
          transaction->setErrorStatus();
          return;
        }

        CROW_LOG_DEBUG << "GetObject returned objects "
                       << interface_names.size();

        for (const std::pair<std::string, std::vector<std::string>> &object :
             interface_names) {
          find_action_on_interface(transaction, object.first);
        }
      },
      "xyz.openbmc_project.ObjectMapper", "/xyz/openbmc_project/object_mapper",
      "xyz.openbmc_project.ObjectMapper", "GetObject", object_path,
      std::array<std::string, 0>());
}

void handle_list(crow::response &res, const std::string &object_path) {
  crow::connections::system_bus->async_method_call(
      [&res](const boost::system::error_code ec,
             std::vector<std::string> &object_paths) {
        if (ec) {
          res.result(boost::beast::http::status::internal_server_error);
        } else {
          res.json_value = {{"status", "ok"},
                            {"message", "200 OK"},
                            {"data", std::move(object_paths)}};
        }
        res.end();
      },
      "xyz.openbmc_project.ObjectMapper", "/xyz/openbmc_project/object_mapper",
      "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths", object_path,
      static_cast<int32_t>(99), std::array<std::string, 0>());
}

void handle_enumerate(crow::response &res, const std::string &object_path) {
  crow::connections::system_bus->async_method_call(
      [&res, object_path{std::string(object_path)} ](
          const boost::system::error_code ec,
          const GetSubTreeType &object_names) {
        if (ec) {
          res.json_value = {{"message", "200 OK"},
                            {"status", "ok"},
                            {"data", nlohmann::json::object()}};

          res.end();
          return;
        }

        boost::container::flat_set<std::string> connections;

        for (const auto &object : object_names) {
          for (const auto &connection : object.second) {
            connections.insert(connection.first);
          }
        }

        if (connections.size() <= 0) {
          res.result(boost::beast::http::status::not_found);
          res.end();
          return;
        }
        auto transaction =
            std::make_shared<nlohmann::json>(nlohmann::json::object());
        for (const std::string &connection : connections) {
          get_managed_objects_for_enumerate(object_path, connection, res,
                                            transaction);
        }
      },
      "xyz.openbmc_project.ObjectMapper", "/xyz/openbmc_project/object_mapper",
      "xyz.openbmc_project.ObjectMapper", "GetSubTree", object_path, (int32_t)0,
      std::array<std::string, 0>());
}

void handle_get(crow::response &res, const std::string &object_path,
                const std::string &dest_property) {
  std::shared_ptr<std::string> property_name =
      std::make_shared<std::string>(dest_property);
  using GetObjectType =
      std::vector<std::pair<std::string, std::vector<std::string>>>;
  crow::connections::system_bus->async_method_call(
      [&res, object_path, property_name](const boost::system::error_code ec,
                                         const GetObjectType &object_names) {
        if (ec || object_names.size() <= 0) {
          res.result(boost::beast::http::status::not_found);
          res.end();
          return;
        }
        std::shared_ptr<nlohmann::json> response =
            std::make_shared<nlohmann::json>(nlohmann::json::object());
        // The mapper should never give us an empty interface names list, but
        // check anyway
        for (const std::pair<std::string, std::vector<std::string>> connection :
             object_names) {
          const std::vector<std::string> &interfaceNames = connection.second;

          if (interfaceNames.size() <= 0) {
            res.result(boost::beast::http::status::not_found);
            res.end();
            return;
          }

          for (const std::string &interface : interfaceNames) {
            crow::connections::system_bus->async_method_call(
                [&res, response, property_name](
                    const boost::system::error_code ec,
                    const std::vector<std::pair<
                        std::string, DbusRestVariantType>> &properties) {
                  if (ec) {
                    CROW_LOG_ERROR << "Bad dbus request error: " << ec;
                  } else {
                    for (const std::pair<std::string, DbusRestVariantType>
                             &property : properties) {
                      // if property name is empty, or matches our search query,
                      // add it to the response json

                      if (property_name->empty()) {
                        mapbox::util::apply_visitor(
                            [&response, &property](auto &&val) {
                              (*response)[property.first] = val;
                            },
                            property.second);
                      } else if (property.first == *property_name) {
                        mapbox::util::apply_visitor(
                            [&response](auto &&val) { (*response) = val; },
                            property.second);
                      }
                    }
                  }
                  if (response.use_count() == 1) {
                    res.json_value = {{"status", "ok"},
                                      {"message", "200 OK"},
                                      {"data", *response}};

                    res.end();
                  }
                },
                connection.first, object_path,
                "org.freedesktop.DBus.Properties", "GetAll", interface);
          }
        }
      },
      "xyz.openbmc_project.ObjectMapper", "/xyz/openbmc_project/object_mapper",
      "xyz.openbmc_project.ObjectMapper", "GetObject", object_path,
      std::array<std::string, 0>());
}

struct AsyncPutRequest {
  AsyncPutRequest(crow::response &res) : res(res) {
    res.json_value = {
        {"status", "ok"}, {"message", "200 OK"}, {"data", nullptr}};
  }
  ~AsyncPutRequest() {
    if (res.result() == boost::beast::http::status::internal_server_error) {
      // Reset the json object to clear out any data that made it in before the
      // error happened
      // todo(ed) handle error condition with proper code
      res.json_value = nlohmann::json::object();
    }

    if (res.json_value.empty()) {
      res.result(boost::beast::http::status::forbidden);
      res.json_value = {
          {"status", "error"},
          {"message", "403 Forbidden"},
          {"data",
           {{"message",
             "The specified property cannot be created: " + propertyName}}}};
    }

    res.end();
  }

  void setErrorStatus() {
    res.result(boost::beast::http::status::internal_server_error);
  }

  crow::response &res;
  std::string objectPath;
  std::string propertyName;
  nlohmann::json propertyValue;
};

void handle_put(const crow::request &req, crow::response &res,
                const std::string &objectPath,
                const std::string &destProperty) {
  nlohmann::json request_dbus_data =
      nlohmann::json::parse(req.body, nullptr, false);

  if (request_dbus_data.is_discarded()) {
    res.result(boost::beast::http::status::bad_request);
    res.end();
    return;
  }

  nlohmann::json::const_iterator property_it = request_dbus_data.find("data");
  if (property_it == request_dbus_data.end()) {
    res.result(boost::beast::http::status::bad_request);
    res.end();
    return;
  }
  const nlohmann::json &propertySetValue = *property_it;
  auto transaction = std::make_shared<AsyncPutRequest>(res);
  transaction->objectPath = objectPath;
  transaction->propertyName = destProperty;
  transaction->propertyValue = propertySetValue;

  using GetObjectType =
      std::vector<std::pair<std::string, std::vector<std::string>>>;

  crow::connections::system_bus->async_method_call(
      [transaction](const boost::system::error_code ec,
                    const GetObjectType &object_names) {
        if (!ec && object_names.size() <= 0) {
          transaction->res.result(boost::beast::http::status::not_found);
          return;
        }

        for (const std::pair<std::string, std::vector<std::string>> connection :
             object_names) {
          const std::string &connectionName = connection.first;

          crow::connections::system_bus->async_method_call(
              [ connectionName{std::string(connectionName)}, transaction ](
                  const boost::system::error_code ec,
                  const std::string &introspectXml) {
                if (ec) {
                  CROW_LOG_ERROR
                      << "Introspect call failed with error: " << ec.message()
                      << " on process: " << connectionName;
                  transaction->setErrorStatus();
                  return;
                }
                tinyxml2::XMLDocument doc;

                doc.Parse(introspectXml.c_str());
                tinyxml2::XMLNode *pRoot = doc.FirstChildElement("node");
                if (pRoot == nullptr) {
                  CROW_LOG_ERROR << "XML document failed to parse: "
                                 << introspectXml;
                  transaction->setErrorStatus();
                  return;
                }
                tinyxml2::XMLElement *ifaceNode =
                    pRoot->FirstChildElement("interface");
                while (ifaceNode != nullptr) {
                  const char *interfaceName = ifaceNode->Attribute("name");
                  CROW_LOG_DEBUG << "found interface " << interfaceName;
                  tinyxml2::XMLElement *propNode =
                      ifaceNode->FirstChildElement("property");
                  while (propNode != nullptr) {
                    const char *propertyName = propNode->Attribute("name");
                    CROW_LOG_DEBUG << "Found property " << propertyName;
                    if (propertyName == transaction->propertyName) {
                      const char *argType = propNode->Attribute("type");
                      if (argType != nullptr) {
                        sdbusplus::message::message m =
                            crow::connections::system_bus->new_method_call(
                                connectionName.c_str(),
                                transaction->objectPath.c_str(),
                                "org.freedesktop.DBus.Properties", "Set");
                        m.append(interfaceName, transaction->propertyName);
                        int r = sd_bus_message_open_container(
                            m.get(), SD_BUS_TYPE_VARIANT, argType);
                        if (r < 0) {
                          transaction->setErrorStatus();
                          return;
                        }
                        r = convert_json_to_dbus(m.get(), argType,
                                                 transaction->propertyValue);
                        if (r < 0) {
                          transaction->setErrorStatus();
                          return;
                        }
                        r = sd_bus_message_close_container(m.get());
                        if (r < 0) {
                          transaction->setErrorStatus();
                          return;
                        }

                        crow::connections::system_bus->async_send(
                            m, [transaction](boost::system::error_code ec,
                                             sdbusplus::message::message &m) {
                              CROW_LOG_DEBUG << "sent";
                              if (ec) {
                                transaction->res.json_value["status"] = "error";
                                transaction->res.json_value["message"] =
                                    ec.message();
                              }
                            });
                      }
                    }
                    propNode = propNode->NextSiblingElement("property");
                  }
                  ifaceNode = ifaceNode->NextSiblingElement("interface");
                }
              },
              connectionName, transaction->objectPath,
              "org.freedesktop.DBus.Introspectable", "Introspect");
        }
      },
      "xyz.openbmc_project.ObjectMapper", "/xyz/openbmc_project/object_mapper",
      "xyz.openbmc_project.ObjectMapper", "GetObject", transaction->objectPath,
      std::array<std::string, 0>());
}

template <typename... Middlewares>
void request_routes(Crow<Middlewares...> &app) {
  CROW_ROUTE(app, "/bus/")
      .methods("GET"_method)([](const crow::request &req, crow::response &res) {
        res.json_value = {{"busses", {{{"name", "system"}}}}, {"status", "ok"}};
      });

  CROW_ROUTE(app, "/bus/system/")
      .methods("GET"_method)([](const crow::request &req, crow::response &res) {
        auto myCallback = [&res](const boost::system::error_code ec,
                                 std::vector<std::string> &names) {
          if (ec) {
            CROW_LOG_ERROR << "Dbus call failed with code " << ec;
            res.result(boost::beast::http::status::internal_server_error);
          } else {
            std::sort(names.begin(), names.end());
            nlohmann::json j{{"status", "ok"}};
            auto &objects_sub = j["objects"];
            for (auto &name : names) {
              objects_sub.push_back({{"name", name}});
            }
            res.json_value = std::move(j);
          }
          res.end();
        };
        crow::connections::system_bus->async_method_call(
            std::move(myCallback), "org.freedesktop.DBus", "/",
            "org.freedesktop.DBus", "ListNames");
      });

  CROW_ROUTE(app, "/list/")
      .methods("GET"_method)([](const crow::request &req, crow::response &res) {
        handle_list(res, "/");
      });

  CROW_ROUTE(app, "/xyz/<path>")
      .methods("GET"_method, "PUT"_method,
               "POST"_method)([](const crow::request &req, crow::response &res,
                                 const std::string &path) {
        std::string object_path = "/xyz/" + path;

        // Trim any trailing "/" at the end
        if (boost::ends_with(object_path, "/")) {
          object_path.pop_back();
        }

        // If accessing a single attribute, fill in and update object_path,
        // otherwise leave dest_property blank
        std::string dest_property = "";
        const char *attr_seperator = "/attr/";
        size_t attr_position = path.find(attr_seperator);
        if (attr_position != path.npos) {
          object_path = "/xyz/" + path.substr(0, attr_position);
          dest_property = path.substr(attr_position + strlen(attr_seperator),
                                      path.length());
        }

        if (req.method() == "POST"_method) {
          constexpr const char *action_seperator = "/action/";
          size_t action_position = path.find(action_seperator);
          if (action_position != path.npos) {
            object_path = "/xyz/" + path.substr(0, action_position);
            std::string post_property = path.substr(
                (action_position + strlen(action_seperator)), path.length());
            handle_action(req, res, object_path, post_property);
            return;
          }
        } else if (req.method() == "GET"_method) {
          if (boost::ends_with(object_path, "/enumerate")) {
            object_path.erase(object_path.end() - 10, object_path.end());
            handle_enumerate(res, object_path);
          } else if (boost::ends_with(object_path, "/list")) {
            object_path.erase(object_path.end() - 5, object_path.end());
            handle_list(res, object_path);
          } else {
            handle_get(res, object_path, dest_property);
          }
          return;
        } else if (req.method() == "PUT"_method) {
          handle_put(req, res, object_path, dest_property);
          return;
        }

        res.result(boost::beast::http::status::method_not_allowed);
        res.end();
      });

  CROW_ROUTE(app, "/bus/system/<str>/")
      .methods("GET"_method)([](const crow::request &req, crow::response &res,
                                const std::string &connection) {
        std::shared_ptr<nlohmann::json> transaction;
        introspect_objects(res, connection, "/", transaction);
      });

  CROW_ROUTE(app, "/download/dump/<str>/")
      .methods("GET"_method)([](const crow::request &req, crow::response &res,
                                const std::string &dumpId) {
        std::regex validFilename("^[\\w\\- ]+(\\.?[\\w\\- ]+)$");
        if (!std::regex_match(dumpId, validFilename)) {
          res.result(boost::beast::http::status::not_found);
          res.end();
          return;
        }
        std::experimental::filesystem::path loc(
            "/var/lib/phosphor-debug-collector/dumps");

        loc += dumpId;

        if (!std::experimental::filesystem::exists(loc) ||
            !std::experimental::filesystem::is_directory(loc)) {
          res.result(boost::beast::http::status::not_found);
          res.end();
          return;
        }
        std::experimental::filesystem::directory_iterator files(loc);
        for (auto &file : files) {
          std::ifstream readFile(file.path());
          if (readFile.good()) {
            continue;
          }
          res.add_header("Content-Type", "application/octet-stream");
          res.body() = {std::istreambuf_iterator<char>(readFile),
                        std::istreambuf_iterator<char>()};
          res.end();
        }
        res.result(boost::beast::http::status::not_found);
        res.end();
        return;
      });

  CROW_ROUTE(app, "/bus/system/<str>/<path>")
      .methods("GET"_method)([](const crow::request &req, crow::response &res,
                                const std::string &process_name,
                                const std::string &requested_path) {
        std::vector<std::string> strs;
        boost::split(strs, requested_path, boost::is_any_of("/"));
        std::string object_path;
        std::string interface_name;
        std::string method_name;
        auto it = strs.begin();
        if (it == strs.end()) {
          object_path = "/";
        }
        while (it != strs.end()) {
          // Check if segment contains ".".  If it does, it must be an
          // interface
          if (it->find(".") != std::string::npos) {
            break;
            // THis check is neccesary as the trailing slash gets parsed as
            // part of our <path> specifier above, which causes the normal
            // trailing backslash redirector to fail.
          } else if (!it->empty()) {
            object_path += "/" + *it;
          }
          it++;
        }
        if (it != strs.end()) {
          interface_name = *it;
          it++;

          // after interface, we might have a method name
          if (it != strs.end()) {
            method_name = *it;
            it++;
          }
        }
        if (it != strs.end()) {
          // if there is more levels past the method name, something went
          // wrong, return not found
          res.result(boost::beast::http::status::not_found);
          res.end();
          return;
        }
        if (interface_name.empty()) {
          crow::connections::system_bus->async_method_call(
              [&, process_name, object_path](
                  const boost::system::error_code ec,
                  const std::string &introspect_xml) {
                if (ec) {
                  CROW_LOG_ERROR
                      << "Introspect call failed with error: " << ec.message()
                      << " on process: " << process_name
                      << " path: " << object_path << "\n";

                } else {
                  tinyxml2::XMLDocument doc;

                  doc.Parse(introspect_xml.c_str());
                  tinyxml2::XMLNode *pRoot = doc.FirstChildElement("node");
                  if (pRoot == nullptr) {
                    CROW_LOG_ERROR << "XML document failed to parse "
                                   << process_name << " " << object_path
                                   << "\n";
                    res.json_value = {{"status", "XML parse error"}};
                    res.result(
                        boost::beast::http::status::internal_server_error);
                  } else {
                    nlohmann::json interfaces_array = nlohmann::json::array();
                    tinyxml2::XMLElement *interface =
                        pRoot->FirstChildElement("interface");

                    while (interface != nullptr) {
                      std::string iface_name = interface->Attribute("name");
                      interfaces_array.push_back({{"name", iface_name}});

                      interface = interface->NextSiblingElement("interface");
                    }
                    res.json_value = {{"status", "ok"},
                                      {"bus_name", process_name},
                                      {"interfaces", interfaces_array},
                                      {"object_path", object_path}};
                  }
                }
                res.end();
              },
              process_name, object_path, "org.freedesktop.DBus.Introspectable",
              "Introspect");
        } else {
          crow::connections::system_bus->async_method_call(
              [
                    &, process_name, object_path,
                    interface_name{std::move(interface_name)}
              ](const boost::system::error_code ec,
                const std::string &introspect_xml) {
                if (ec) {
                  CROW_LOG_ERROR
                      << "Introspect call failed with error: " << ec.message()
                      << " on process: " << process_name
                      << " path: " << object_path << "\n";

                } else {
                  tinyxml2::XMLDocument doc;

                  doc.Parse(introspect_xml.c_str());
                  tinyxml2::XMLNode *pRoot = doc.FirstChildElement("node");
                  if (pRoot == nullptr) {
                    CROW_LOG_ERROR << "XML document failed to parse "
                                   << process_name << " " << object_path
                                   << "\n";
                    res.result(
                        boost::beast::http::status::internal_server_error);

                  } else {
                    tinyxml2::XMLElement *node =
                        pRoot->FirstChildElement("node");

                    // if we know we're the only call, build the json directly
                    nlohmann::json methods_array = nlohmann::json::array();
                    nlohmann::json signals_array = nlohmann::json::array();
                    tinyxml2::XMLElement *interface =
                        pRoot->FirstChildElement("interface");

                    while (interface != nullptr) {
                      std::string iface_name = interface->Attribute("name");

                      if (iface_name == interface_name) {
                        tinyxml2::XMLElement *methods =
                            interface->FirstChildElement("method");
                        while (methods != nullptr) {
                          nlohmann::json args_array = nlohmann::json::array();
                          tinyxml2::XMLElement *arg =
                              methods->FirstChildElement("arg");
                          while (arg != nullptr) {
                            args_array.push_back(
                                {{"name", arg->Attribute("name")},
                                 {"type", arg->Attribute("type")},
                                 {"direction", arg->Attribute("direction")}});
                            arg = arg->NextSiblingElement("arg");
                          }
                          methods_array.push_back(
                              {{"name", methods->Attribute("name")},
                               {"uri", "/bus/system/" + process_name +
                                           object_path + "/" + interface_name +
                                           "/" + methods->Attribute("name")},
                               {"args", args_array}});
                          methods = methods->NextSiblingElement("method");
                        }
                        tinyxml2::XMLElement *signals =
                            interface->FirstChildElement("signal");
                        while (signals != nullptr) {
                          nlohmann::json args_array = nlohmann::json::array();

                          tinyxml2::XMLElement *arg =
                              signals->FirstChildElement("arg");
                          while (arg != nullptr) {
                            std::string name = arg->Attribute("name");
                            std::string type = arg->Attribute("type");
                            args_array.push_back({
                                {"name", name},
                                {"type", type},
                            });
                            arg = arg->NextSiblingElement("arg");
                          }
                          signals_array.push_back(
                              {{"name", signals->Attribute("name")},
                               {"args", args_array}});
                          signals = signals->NextSiblingElement("signal");
                        }

                        res.json_value = {
                            {"status", "ok"},
                            {"bus_name", process_name},
                            {"interface", interface_name},
                            {"methods", methods_array},
                            {"object_path", object_path},
                            {"properties", nlohmann::json::object()},
                            {"signals", signals_array}};

                        break;
                      }

                      interface = interface->NextSiblingElement("interface");
                    }
                    if (interface == nullptr) {
                      // if we got to the end of the list and never found a
                      // match, throw 404
                      res.result(boost::beast::http::status::not_found);
                    }
                  }
                }
                res.end();
              },
              process_name, object_path, "org.freedesktop.DBus.Introspectable",
              "Introspect");
        }
      });
}
}  // namespace openbmc_mapper
}  // namespace crow
