#include "config.h"
#include "ConfigProcessor.h"
#include "YAMLParser.h"

#include <sys/utsname.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <boost/algorithm/string.hpp>
#include <Poco/DOM/Text.h>
#include <Poco/DOM/Attr.h>
#include <Poco/DOM/Comment.h>
#include <Poco/XML/XMLWriter.h>
#include <Poco/Util/XMLConfiguration.h>
#include <Common/ZooKeeper/ZooKeeperNodeCache.h>
#include <Common/ZooKeeper/KeeperException.h>
#include <Common/StringUtils/StringUtils.h>
#include <Common/Exception.h>
#include <Common/getResource.h>
#include <Common/XMLUtils.h>
#include <Common/logger_useful.h>
#include <base/errnoToString.h>
#include <base/sort.h>
#include <IO/WriteBufferFromString.h>
#include <IO/Operators.h>
#include <Poco/DOM/CDATASection.h>
#include <Poco/XML/XMLWriter.h>
#include <Common/FoundationDB/FoundationDBCommon.h>
#include <Common/FoundationDB/protos/MetadataConfigParam.pb.h>

#define PREPROCESSED_SUFFIX "-preprocessed"

namespace fs = std::filesystem;

using namespace Poco::XML;

namespace DB
{

namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int CANNOT_LOAD_CONFIG;
}

/// For cutting preprocessed path to this base
static std::string main_config_path;


bool ConfigProcessor::isPreprocessedFile(const std::string & path)
{
    return endsWith(fs::path(path).stem(), PREPROCESSED_SUFFIX);
}


ConfigProcessor::ConfigProcessor(
    const std::string & path_,
    bool throw_on_bad_incl_,
    bool log_to_console,
    const Substitutions & substitutions_)
    : path(path_)
    , throw_on_bad_incl(throw_on_bad_incl_)
    , substitutions(substitutions_)
    /// We need larger name pool to allow to support vast amount of users in users.xml files for ClickHouse.
    /// Size is prime because Poco::XML::NamePool uses bad (inefficient, low quality)
    ///  hash function internally, and its size was prime by default.
    , name_pool(new Poco::XML::NamePool(65521))
    , dom_parser(name_pool)
{
    if (log_to_console && !Poco::Logger::has("ConfigProcessor"))
    {
        channel_ptr = new Poco::ConsoleChannel;
        log = &Poco::Logger::create("ConfigProcessor", channel_ptr.get(), Poco::Message::PRIO_TRACE);
    }
    else
    {
        log = &Poco::Logger::get("ConfigProcessor");
    }
}

ConfigProcessor::~ConfigProcessor()
{
    if (channel_ptr) /// This means we have created a new console logger in the constructor.
        Poco::Logger::destroy("ConfigProcessor");
}


/// Vector containing the name of the element and a sorted list of attribute names and values
/// (except "remove" and "replace" attributes).
/// Serves as a unique identifier of the element contents for comparison.
using ElementIdentifier = std::vector<std::string>;

using NamedNodeMapPtr = Poco::AutoPtr<Poco::XML::NamedNodeMap>;
/// NOTE getting rid of iterating over the result of Node.childNodes() call is a good idea
/// because accessing the i-th element of this list takes O(i) time.
using NodeListPtr = Poco::AutoPtr<Poco::XML::NodeList>;

static ElementIdentifier getElementIdentifier(Node * element)
{
    const NamedNodeMapPtr attrs = element->attributes();
    std::vector<std::pair<std::string, std::string>> attrs_kv;
    for (const Node * node = attrs->item(0); node; node = node->nextSibling())
    {
        std::string name = node->nodeName();
        const auto * subst_name_pos = std::find(ConfigProcessor::SUBSTITUTION_ATTRS.begin(), ConfigProcessor::SUBSTITUTION_ATTRS.end(), name);
        if (name == "replace" || name == "remove" ||
            subst_name_pos != ConfigProcessor::SUBSTITUTION_ATTRS.end())
            continue;
        std::string value = node->nodeValue();
        attrs_kv.push_back(std::make_pair(name, value));
    }
    ::sort(attrs_kv.begin(), attrs_kv.end());

    ElementIdentifier res;
    res.push_back(element->nodeName());
    for (const auto & attr : attrs_kv)
    {
        res.push_back(attr.first);
        res.push_back(attr.second);
    }

    return res;
}

static Node * getRootNode(Document * document)
{
    return XMLUtils::getRootNode(document);
}

static bool allWhitespace(const std::string & s)
{
    return s.find_first_not_of(" \t\n\r") == std::string::npos;
}

static void deleteAttributesRecursive(Node * root)
{
    const NodeListPtr children = root->childNodes();
    std::vector<Node *> children_to_delete;

    for (Node * child = children->item(0); child; child = child->nextSibling())
    {
        if (child->nodeType() == Node::ELEMENT_NODE)
        {
            Element & child_element = dynamic_cast<Element &>(*child);

            if (child_element.hasAttribute("replace"))
                child_element.removeAttribute("replace");

            if (child_element.hasAttribute("remove"))
                children_to_delete.push_back(child);
            else
                deleteAttributesRecursive(child);
        }
    }

    for (auto * child : children_to_delete)
    {
        root->removeChild(child);
    }
}

/**
 * Transform poco::xml::document object to fine-grained kvs.
 * @param root root node of document object
 * @param flag mark whether the current node has a sibling node with the same name
 * @param index if flag = true, the prefix need to include an index number
 */
void ConfigProcessor::transformXMLToKV(const Node * root, std::string prefix, ConfigParamKVs & kvs, bool flag, size_t index)
{
    if (root->nodeType() == Node::TEXT_NODE || root->nodeType() == Node::CDATA_SECTION_NODE)
    {
        /// Current node is a text node, reach the destination
        if (!allWhitespace(root->getNodeValue()))
        {
            std::unique_ptr<ConfigParamKV> temp(new FoundationDB::Proto::MetadataConfigParam());
            temp->set_name(prefix);
            temp->set_value(root->getNodeValue());
            kvs.emplace_back(std::move(temp));
        }
    }

    /// 1. Update prefix.
    std::string root_name = root->nodeName();

    if (root -> hasAttributes())
        throw Poco::Exception("Configuration \"" + root_name + "\" cannot contain attributes when foundationDB is enabled.");

    /// Ignore config parameters prefixed with users, profiles, quotas, dictionaries, functions, which will be persisted by the corresponding modules.
    if (root_name == "users" || root_name == "profiles" || root_name == "quotas" || root_name == "dictionaries" || root_name == "functions")
        return;

    /// Mark whether the current node need to add a prefix
    bool cur_flag = false;
    if (root->nodeType() == Node::ELEMENT_NODE && (root_name != "clickhouse" && root_name != "yandex"))
        cur_flag = true;
    /// Add current node name to the prefix.
    if (cur_flag)
    {
        if (prefix.empty())
            prefix += root_name;
        else
            prefix = prefix + '.' + root_name;
    }
    /// If the current node has a sibling node with the same name, Distinguish it by [$num].
    if (flag)
    {
        prefix = prefix + '[' + std::to_string(index) + ']';
    }
    /// 2. Traverse child nodes.
    const NodeListPtr children = root->childNodes();
    for (size_t i = 0, size = children->length(), pos = -1; i < size; ++i)
    {
        Node * child = children->item(i);
        bool child_flag = false;
        if (child->nodeType() == Node::ELEMENT_NODE)
        {
            /// Whether the sibling node behind the child node has the same name.
            if (i + 2 < size)
            {
                if (child->nodeName() == child->nextSibling()->nextSibling()->nodeName())
                {
                    child_flag = true;
                    pos++;
                }
            }
            /// Whether the previous sibling node of the child node has the same name.
            else if (i >= 2)
            {
                if (child->nodeName() == child->previousSibling()->previousSibling()->nodeName())
                {
                    child_flag = true;
                    pos++;
                }
            }
        }
        transformXMLToKV(child, prefix, kvs, child_flag, pos);
    }
}

static std::string getNodePathFromName(std::string name)
{
    std::string::size_type pos;
    while ((pos = name.find('.')) != std::string::npos)
    {
        name.replace(pos, 1, "/");
    }
    return name;
}


/// If the node does not exist, create it, if the node exists, update it's value.
static Node * createNode(XMLDocumentPtr doc, ConfigurationPtr configuration, ConfigParamKV * item, std::string & name, std::string & path)
{
    /// The current node does not exist.
    if (!configuration->has(name))
    {
        std::string::size_type pos = path.find_last_of('/');
        std::string curr_node_name = path.substr(pos + 1, path.size() - pos - 1);
        std::string parent_path = path.substr(0, pos);
        std::string parent_name;

        pos = curr_node_name.find('[');
        if (pos != std::string::npos)
            curr_node_name.erase(pos, 3);

        pos = name.find_last_of('.');
        if (pos != std::string::npos)
            parent_name = name.substr(0, pos);

        Node * parent;
        /// 1. If the parent node exists, find it, otherwise recursively create the parent node.
        if (configuration->has(parent_name))
        {
            parent = doc->getNodeByPath(parent_path);
        }
        else
        {
            ConfigParamKV parent_item;
            parent_item.set_name(parent_name);
            parent = createNode(doc, configuration, &parent_item, parent_name, parent_path);
        }
        /// 2. Create the child node.
        Poco::AutoPtr<Poco::XML::Element> temp = doc->createElement(curr_node_name);
        if (!item->value().empty())
        {
            if (item->value().find('<') != std::string::npos)
            {
                Poco::AutoPtr<Poco::XML::CDATASection> cdata = doc->createCDATASection(item->value());
                temp->appendChild(cdata);
            }
            else
            {
                Poco::AutoPtr<Poco::XML::Text> txt = doc->createTextNode(item->value());
                temp->appendChild(txt);
            }
        }
        parent->appendChild(temp);
        return temp;
    }
    /// The current node exists, return this node.
    else
    {
        Element * cur = dynamic_cast<Element *>(doc->getNodeByPath(path));
        ///  If the current node has a value, update the value.
        if (!item->value().empty())
        {
            if (item->value().find('<') != std::string::npos)
            {
                Poco::AutoPtr<Poco::XML::CDATASection> cdata = doc->createCDATASection(item->value());
                if (cur->hasChildNodes())
                {
                    Node * old = cur->firstChild();
                    cur->replaceChild(cdata, old);
                }
                else
                {
                    cur->appendChild(cdata);
                }
            }
            else
            {
                Poco::AutoPtr<Poco::XML::Text> txt = doc->createTextNode(item->value());
                if (cur->hasChildNodes())
                {
                    Node * old = cur->firstChild();
                    cur->replaceChild(txt, old);
                }
                else
                {
                    cur->appendChild(txt);
                }
            }
        }
        return cur;
    }
}

static XMLDocumentPtr transformKVToXML(const ConfigParamKVs & kvs)
{
    XMLDocumentPtr config = new Poco::XML::Document;
    Poco::AutoPtr<Poco::XML::Element> root = config->createElement("clickhouse");
    config->appendChild(root);
    ConfigurationPtr configuration(new Poco::Util::XMLConfiguration(config));
    std::string path;
    std::string name;
    for (const auto & kv : kvs)
    {
        ConfigParamKV temp = *kv;
        path = "clickhouse/" + getNodePathFromName(kv->name());
        name = kv->name();
        createNode(config, configuration, &temp, name, path);
    }
    return config;
}

static void mergeAttributes(Element & config_element, Element & with_element)
{
    auto * with_element_attributes = with_element.attributes();

    for (size_t i = 0; i < with_element_attributes->length(); ++i)
    {
        auto * attr = with_element_attributes->item(i);
        config_element.setAttribute(attr->nodeName(), attr->getNodeValue());
    }

    with_element_attributes->release();
}

void ConfigProcessor::mergeRecursive(XMLDocumentPtr config, Node * config_root, const Node * with_root)
{
    const NodeListPtr with_nodes = with_root->childNodes();
    using ElementsByIdentifier = std::multimap<ElementIdentifier, Node *>;
    ElementsByIdentifier config_element_by_id;
    for (Node * node = config_root->firstChild(); node;)
    {
        Node * next_node = node->nextSibling();
        /// Remove text from the original config node.
        if (node->nodeType() == Node::TEXT_NODE && !allWhitespace(node->getNodeValue()))
        {
            config_root->removeChild(node);
        }
        else if (node->nodeType() == Node::ELEMENT_NODE)
        {
            config_element_by_id.insert(ElementsByIdentifier::value_type(getElementIdentifier(node), node));
        }
        node = next_node;
    }

    Node * next_with_node = nullptr;
    for (Node * with_node = with_nodes->item(0); with_node; with_node = next_with_node)
    {
        next_with_node = with_node->nextSibling();
        bool merged = false;
        bool remove = false;
        if (with_node->nodeType() == Node::ELEMENT_NODE)
        {
            Element & with_element = dynamic_cast<Element &>(*with_node);
            remove = with_element.hasAttribute("remove");
            bool replace = with_element.hasAttribute("replace");

            if (remove && replace)
                throw Poco::Exception("both remove and replace attributes set for element <" + with_node->nodeName() + ">");

            ElementsByIdentifier::iterator it = config_element_by_id.find(getElementIdentifier(with_node));

            if (it != config_element_by_id.end())
            {
                Node * config_node = it->second;
                config_element_by_id.erase(it);

                if (remove)
                {
                    config_root->removeChild(config_node);
                }
                else if (replace)
                {
                    with_element.removeAttribute("replace");
                    NodePtr new_node = config->importNode(with_node, true);
                    config_root->replaceChild(new_node, config_node);
                }
                else
                {
                    Element & config_element = dynamic_cast<Element &>(*config_node);

                    mergeAttributes(config_element, with_element);
                    mergeRecursive(config, config_node, with_node);
                }
                merged = true;
            }
        }
        if (!merged && !remove)
        {
            /// Since we didn't find a pair to this node in default config, we will paste it as is.
            /// But it may have some child nodes which have attributes like "replace" or "remove".
            /// They are useless in preprocessed configuration.
            deleteAttributesRecursive(with_node);
            NodePtr new_node = config->importNode(with_node, true);
            config_root->appendChild(new_node);
        }
    }
}

void ConfigProcessor::merge(XMLDocumentPtr config, XMLDocumentPtr with)
{
    Node * config_root = getRootNode(config.get());
    Node * with_root = getRootNode(with.get());

    std::string config_root_node_name = config_root->nodeName();
    std::string merged_root_node_name = with_root->nodeName();

    /// For compatibility, we treat 'yandex' and 'clickhouse' equivalent.
    /// See https://clickhouse.com/blog/en/2021/clickhouse-inc/

    if (config_root_node_name != merged_root_node_name
        && !((config_root_node_name == "yandex" || config_root_node_name == "clickhouse")
            && (merged_root_node_name == "yandex" || merged_root_node_name == "clickhouse")))
    {
        throw Poco::Exception("Root element doesn't have the corresponding root element as the config file."
            " It must be <" + config_root->nodeName() + ">");
    }

    mergeRecursive(config, config_root, with_root);
}

void ConfigProcessor::doIncludesRecursive(
        XMLDocumentPtr config,
        XMLDocumentPtr include_from,
        Node * node,
        zkutil::ZooKeeperNodeCache * zk_node_cache,
        const zkutil::EventPtr & zk_changed_event,
        std::unordered_set<std::string> & contributing_zk_paths)
{
    if (node->nodeType() == Node::TEXT_NODE)
    {
        for (auto & substitution : substitutions)
        {
            std::string value = node->nodeValue();

            bool replace_occured = false;
            size_t pos;
            while ((pos = value.find(substitution.first)) != std::string::npos)
            {
                value.replace(pos, substitution.first.length(), substitution.second);
                replace_occured = true;
            }

            if (replace_occured)
                node->setNodeValue(value);
        }
    }

    if (node->nodeType() != Node::ELEMENT_NODE)
        return;

    std::map<std::string, const Node *> attr_nodes;
    NamedNodeMapPtr attributes = node->attributes();
    size_t substs_count = 0;
    for (const auto & attr_name : SUBSTITUTION_ATTRS)
    {
        const auto * subst = attributes->getNamedItem(attr_name);
        attr_nodes[attr_name] = subst;
        substs_count += static_cast<size_t>(subst != nullptr);
    }

    if (substs_count > 1) /// only one substitution is allowed
        throw Poco::Exception("More than one substitution attribute is set for element <" + node->nodeName() + ">");

    if (node->nodeName() == "include")
    {
        if (node->hasChildNodes())
            throw Poco::Exception("<include> element must have no children");
        if (substs_count == 0)
            throw Poco::Exception("No substitution attributes set for element <include>, must have exactly one");
    }

    /// Replace the original contents, not add to it.
    bool replace = attributes->getNamedItem("replace");

    bool included_something = false;

    auto process_include = [&](const Node * include_attr, const std::function<const Node * (const std::string &)> & get_node, const char * error_msg)
    {
        const std::string & name = include_attr->getNodeValue();
        const Node * node_to_include = get_node(name);
        if (!node_to_include)
        {
            if (attributes->getNamedItem("optional"))
                node->parentNode()->removeChild(node);
            else if (throw_on_bad_incl)
                throw Poco::Exception(error_msg + name);
            else
            {
                if (node->nodeName() == "include")
                    node->parentNode()->removeChild(node);

                LOG_WARNING(log, "{}{}", error_msg, name);
            }
        }
        else
        {
            /// Replace the whole node not just contents.
            if (node->nodeName() == "include")
            {
                const NodeListPtr children = node_to_include->childNodes();
                Node * next_child = nullptr;
                for (Node * child = children->item(0); child; child = next_child)
                {
                    next_child = child->nextSibling();
                    NodePtr new_node = config->importNode(child, true);
                    node->parentNode()->insertBefore(new_node, node);
                }

                node->parentNode()->removeChild(node);
            }
            else
            {
                Element & element = dynamic_cast<Element &>(*node);

                for (const auto & attr_name : SUBSTITUTION_ATTRS)
                    element.removeAttribute(attr_name);

                if (replace)
                {
                    while (Node * child = node->firstChild())
                        node->removeChild(child);

                    element.removeAttribute("replace");
                }

                const NodeListPtr children = node_to_include->childNodes();
                Node * next_child = nullptr;
                for (Node * child = children->item(0); child; child = next_child)
                {
                    next_child = child->nextSibling();
                    NodePtr new_node = config->importNode(child, true);
                    node->appendChild(new_node);
                }

                const NamedNodeMapPtr from_attrs = node_to_include->attributes();
                Node * next_attr = nullptr;
                for (Node * attr = from_attrs->item(0); attr; attr = next_attr)
                {
                    next_attr = attr->nextSibling();
                    element.setAttributeNode(dynamic_cast<Attr *>(config->importNode(attr, true)));
                }

                included_something = true;
            }
        }
    };

    if (attr_nodes["incl"]) // we have include subst
    {
        auto get_incl_node = [&](const std::string & name)
        {
            return include_from ? getRootNode(include_from.get())->getNodeByPath(name) : nullptr;
        };

        process_include(attr_nodes["incl"], get_incl_node, "Include not found: ");
    }

    if (attr_nodes["from_zk"]) /// we have zookeeper subst
    {
        contributing_zk_paths.insert(attr_nodes["from_zk"]->getNodeValue());

        if (zk_node_cache)
        {
            XMLDocumentPtr zk_document;
            auto get_zk_node = [&](const std::string & name) -> const Node *
            {
                zkutil::ZooKeeperNodeCache::ZNode znode = zk_node_cache->get(name, zk_changed_event);
                if (!znode.exists)
                    return nullptr;

                /// Enclose contents into a fake <from_zk> tag to allow pure text substitutions.
                zk_document = dom_parser.parseString("<from_zk>" + znode.contents + "</from_zk>");
                return getRootNode(zk_document.get());
            };

            process_include(attr_nodes["from_zk"], get_zk_node, "Could not get ZooKeeper node: ");
        }
    }

    if (attr_nodes["from_env"]) /// we have env subst
    {
        XMLDocumentPtr env_document;
        auto get_env_node = [&](const std::string & name) -> const Node *
        {
            const char * env_val = std::getenv(name.c_str()); // NOLINT(concurrency-mt-unsafe) // this is safe on Linux glibc/Musl, but potentially not safe on other platforms
            if (env_val == nullptr)
                return nullptr;

            env_document = dom_parser.parseString("<from_env>" + std::string{env_val} + "</from_env>");

            return getRootNode(env_document.get());
        };

        process_include(attr_nodes["from_env"], get_env_node, "Env variable is not set: ");
    }

    if (included_something)
        doIncludesRecursive(config, include_from, node, zk_node_cache, zk_changed_event, contributing_zk_paths);
    else
    {
        NodeListPtr children = node->childNodes();
        Node * next_child = nullptr;
        for (Node * child = children->item(0); child; child = next_child)
        {
            next_child = child->nextSibling();
            doIncludesRecursive(config, include_from, child, zk_node_cache, zk_changed_event, contributing_zk_paths);
        }
    }
}

ConfigProcessor::Files ConfigProcessor::getConfigMergeFiles(const std::string & config_path)
{
    Files files;

    fs::path merge_dir_path(config_path);
    std::set<std::string> merge_dirs;

    /// Add path_to_config/config_name.d dir
    merge_dir_path.replace_extension("d");
    merge_dirs.insert(merge_dir_path);
    /// Add path_to_config/conf.d dir
    merge_dir_path.replace_filename("conf.d");
    merge_dirs.insert(merge_dir_path);

    for (const std::string & merge_dir_name : merge_dirs)
    {
        if (!fs::exists(merge_dir_name) || !fs::is_directory(merge_dir_name))
            continue;

        for (fs::directory_iterator it(merge_dir_name); it != fs::directory_iterator(); ++it)
        {
            fs::path path(it->path());
            std::string extension = path.extension();
            std::string base_name = path.stem();

            boost::algorithm::to_lower(extension);

            // Skip non-config and temporary files
            if (fs::is_regular_file(path)
                    && (extension == ".xml" || extension == ".conf" || extension == ".yaml" || extension == ".yml")
                    && !startsWith(base_name, "."))
                files.push_back(it->path());
        }
    }

    ::sort(files.begin(), files.end());

    return files;
}

XMLDocumentPtr ConfigProcessor::processConfig(
    bool * has_zk_includes,
    zkutil::ZooKeeperNodeCache * zk_node_cache,
    const zkutil::EventPtr & zk_changed_event,
    bool * has_fdb)
{
    LOG_DEBUG(log, "Processing configuration file '{}'.", path);

    XMLDocumentPtr config;

    if (fs::exists(path))
    {
        fs::path p(path);

        std::string extension = p.extension();
        boost::algorithm::to_lower(extension);

        if (extension == ".yaml" || extension == ".yml")
        {
            config = YAMLParser::parse(path);
        }
        else if (extension == ".xml" || extension == ".conf" || extension.empty())
        {
            config = dom_parser.parse(path);
        }
        else
        {
            throw Exception(ErrorCodes::CANNOT_LOAD_CONFIG, "Unknown format of '{}' config", path);
        }
    }
    else
    {
        /// These embedded files added during build with some cmake magic.
        /// Look at the end of programs/server/CMakeLists.txt.
        std::string embedded_name;
        if (path == "config.xml")
            embedded_name = "embedded.xml";

        if (path == "keeper_config.xml")
            embedded_name = "keeper_embedded.xml";

        /// When we can use config embedded in binary.
        if (!embedded_name.empty())
        {
            auto resource = getResource(embedded_name);
            if (resource.empty())
                throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "Configuration file {} doesn't exist and there is no embedded config", path);
            LOG_DEBUG(log, "There is no file '{}', will use embedded config.", path);
            config = dom_parser.parseMemory(resource.data(), resource.size());
        }
        else
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "Configuration file {} doesn't exist", path);
    }

    std::vector<std::string> contributing_files;
    contributing_files.push_back(path);

    for (auto & merge_file : getConfigMergeFiles(path))
    {
        try
        {
            LOG_DEBUG(log, "Merging configuration file '{}'.", merge_file);

            XMLDocumentPtr with;

            fs::path p(merge_file);
            std::string extension = p.extension();
            boost::algorithm::to_lower(extension);

            if (extension == ".yaml" || extension == ".yml")
            {
                with = YAMLParser::parse(merge_file);
            }
            else
            {
                with = dom_parser.parse(merge_file);
            }

            merge(config, with);

            contributing_files.push_back(merge_file);
        }
        catch (Exception & e)
        {
            e.addMessage("while merging config '" + path + "' with '" + merge_file + "'");
            throw;
        }
        catch (Poco::Exception & e)
        {
            throw Poco::Exception("Failed to merge config with '" + merge_file + "': " + e.displayText());
        }
    }

    std::unordered_set<std::string> contributing_zk_paths;
    try
    {
        Node * node = getRootNode(config.get())->getNodeByPath("include_from");

        XMLDocumentPtr include_from;
        std::string include_from_path;
        if (node)
        {
            /// if we include_from env or zk.
            doIncludesRecursive(config, nullptr, node, zk_node_cache, zk_changed_event, contributing_zk_paths);
            include_from_path = node->innerText();
        }
        else
        {
            std::string default_path = "/etc/metrika.xml";
            if (fs::exists(default_path))
                include_from_path = default_path;
        }
        if (!include_from_path.empty())
        {
            LOG_DEBUG(log, "Including configuration file '{}'.", include_from_path);

            contributing_files.push_back(include_from_path);
            include_from = dom_parser.parse(include_from_path);
        }

        doIncludesRecursive(config, include_from, getRootNode(config.get()), zk_node_cache, zk_changed_event, contributing_zk_paths);
    }
    catch (Exception & e)
    {
        e.addMessage("while preprocessing config '" + path + "'");
        throw;
    }
    catch (Poco::Exception & e)
    {
        throw Poco::Exception("Failed to preprocess config '" + path + "': " + e.displayText(), e);
    }

    if (has_zk_includes)
        *has_zk_includes = !contributing_zk_paths.empty();

    if (has_fdb)
        *has_fdb = getRootNode(config.get())->getNodeByPath("foundationdb") != nullptr;

    WriteBufferFromOwnString comment;
    comment <<     " This file was generated automatically.\n";
    comment << "     Do not edit it: it is likely to be discarded and generated again before it's read next time.\n";
    comment << "     Files used to generate this file:";
    for (const std::string & contributing_file : contributing_files)
    {
        comment << "\n       " << contributing_file;
    }
    if (zk_node_cache && !contributing_zk_paths.empty())
    {
        comment << "\n     ZooKeeper nodes used to generate this file:";
        for (const std::string & contributing_zk_path : contributing_zk_paths)
            comment << "\n       " << contributing_zk_path;
    }

    comment << "      ";
    NodePtr new_node = config->createTextNode("\n\n");
    config->insertBefore(new_node, config->firstChild());
    new_node = config->createComment(comment.str());
    config->insertBefore(new_node, config->firstChild());

    return config;
}

void ConfigProcessor::pushConfigToFoundationDB(LoadedConfig & loaded_config, std::shared_ptr<MetadataStoreFoundationDB> meta_store)
{
    try
    {
        ConfigParamKVs kvs;
        Node * config_root = getRootNode(loaded_config.preprocessed_xml.get());
        transformXMLToKV(config_root, "", kvs, false);
        meta_store->addBunchConfigParamMeta(kvs);
    }
    catch (DB::FoundationDBException & e)
    {
        throw Poco::Exception("Failed to push config to foundationDB: {}", e.displayText());
    }
}

ConfigProcessor::LoadedConfig ConfigProcessor::loadConfig(bool allow_zk_includes)
{
    bool has_zk_includes;
    bool has_fdb;
    XMLDocumentPtr config_xml = processConfig(&has_zk_includes, nullptr, nullptr, &has_fdb);

    if (has_zk_includes && !allow_zk_includes)
        throw Poco::Exception("Error while loading config '" + path + "': from_zk includes are not allowed!");

    ConfigurationPtr configuration(new Poco::Util::XMLConfiguration(config_xml));

    return LoadedConfig{configuration, has_zk_includes, /* loaded_from_preprocessed = */ false, has_fdb, /* fdb_loaded_from_preprocessed = */ true, config_xml, path};
}

ConfigProcessor::LoadedConfig ConfigProcessor::loadConfigWithZooKeeperIncludes(
        zkutil::ZooKeeperNodeCache & zk_node_cache,
        const zkutil::EventPtr & zk_changed_event,
        bool fallback_to_preprocessed)
{
    XMLDocumentPtr config_xml;
    bool has_zk_includes;
    bool has_fdb;
    bool processed_successfully = false;
    try
    {
        config_xml = processConfig(&has_zk_includes, &zk_node_cache, zk_changed_event, &has_fdb);
        processed_successfully = true;
    }
    catch (const Poco::Exception & ex)
    {
        if (!fallback_to_preprocessed)
            throw;

        const auto * zk_exception = dynamic_cast<const Coordination::Exception *>(ex.nested());
        if (!zk_exception)
            throw;

        LOG_WARNING(log, "Error while processing from_zk config includes: {}. Config will be loaded from preprocessed file: {}", zk_exception->message(), preprocessed_path);

        config_xml = dom_parser.parse(preprocessed_path);
    }

    ConfigurationPtr configuration(new Poco::Util::XMLConfiguration(config_xml));

    return LoadedConfig{configuration, has_zk_includes, !processed_successfully, has_fdb, /* fdb_loaded_from_preprocessed = */ true, config_xml, path};
}

ConfigParamKVs ConfigProcessor::config_kv = std::vector<std::unique_ptr<ConfigParamKV>>();

void ConfigProcessor::loadConfigWithFDB(
        LoadedConfig & loaded_config,
        std::shared_ptr<MetadataStoreFoundationDB> meta_store,
        bool fdb_changed, bool fallback_to_preprocessed)
{
    if (meta_store == nullptr)
    {
        LOG_WARNING(log, "Error MetadataStoreFoundationDB is nullptr while loading configurations from foundationDB.");
        return;
    }
    XMLDocumentPtr config_xml = loaded_config.preprocessed_xml;
    try
    {
        if (fdb_changed)
            config_kv = meta_store->listAllConfigParamMeta();
        XMLDocumentPtr fdb_config_xml = transformKVToXML(config_kv);
        merge(config_xml, fdb_config_xml);
        loaded_config.fdb_loaded_from_preprocessed = false;
    }
    catch (const Poco::Exception & ex)
    {
        if (!fallback_to_preprocessed)
            throw;

        const auto * fdb_exception = dynamic_cast<const DB::FoundationDBException *>(ex.nested());
        if (!fdb_exception)
            throw;

        LOG_WARNING(log, "Error while pulling config from foundationDB: {}. Config will be loaded from preprocessed file: {}", ex.displayText(), preprocessed_path);

        /// Fallback to preprocessed.
        config_xml = dom_parser.parse(preprocessed_path);
        loaded_config.preprocessed_xml = config_xml;
        loaded_config.configuration = new Poco::Util::XMLConfiguration(config_xml);
        loaded_config.fdb_loaded_from_preprocessed = true;
    }
}

void ConfigProcessor::savePreprocessedConfig(const LoadedConfig & loaded_config, std::string preprocessed_dir)
{
    try
    {
        if (preprocessed_path.empty())
        {
            fs::path preprocessed_configs_path("preprocessed_configs/");
            auto new_path = loaded_config.config_path;
            if (new_path.starts_with(main_config_path))
                new_path.erase(0, main_config_path.size());
            std::replace(new_path.begin(), new_path.end(), '/', '_');

            /// If we have config file in YAML format, the preprocessed config will inherit .yaml extension
            /// but will contain config in XML format, so some tools like clickhouse extract-from-config won't work
            new_path = fs::path(new_path).replace_extension(".xml").string();

            if (preprocessed_dir.empty())
            {
                if (!loaded_config.configuration->has("path"))
                {
                    // Will use current directory
                    fs::path parent_path = fs::path(loaded_config.config_path).parent_path();
                    preprocessed_dir = parent_path.string();
                    fs::path fs_new_path(new_path);
                    fs_new_path.replace_filename(fs_new_path.stem().string() + PREPROCESSED_SUFFIX + fs_new_path.extension().string());
                    new_path = fs_new_path.string();
                }
                else
                {
                    fs::path loaded_config_path(loaded_config.configuration->getString("path"));
                    preprocessed_dir = loaded_config_path / preprocessed_configs_path;
                }
            }
            else
            {
                fs::path preprocessed_dir_path(preprocessed_dir);
                preprocessed_dir = (preprocessed_dir_path / preprocessed_configs_path).string();
            }

            preprocessed_path = (fs::path(preprocessed_dir) / fs::path(new_path)).string();
            auto preprocessed_path_parent = fs::path(preprocessed_path).parent_path();
            if (!preprocessed_path_parent.empty())
                fs::create_directories(preprocessed_path_parent);
        }
        DOMWriter writer;
        writer.setNewLine("\n");
        writer.setIndent("    ");
        writer.setOptions(Poco::XML::XMLWriter::PRETTY_PRINT);
        writer.writeNode(preprocessed_path, loaded_config.preprocessed_xml);
        LOG_DEBUG(log, "Saved preprocessed configuration to '{}'.", preprocessed_path);
    }
    catch (Poco::Exception & e)
    {
        LOG_WARNING(log, "Couldn't save preprocessed config to {}: {}", preprocessed_path, e.displayText());
    }
}

void ConfigProcessor::setConfigPath(const std::string & config_path)
{
    main_config_path = config_path;
    if (!main_config_path.ends_with('/'))
        main_config_path += '/';
}

}