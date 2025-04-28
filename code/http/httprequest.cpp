#include "httprequest.h"
#include <fstream>
#include <sys/stat.h>
#include <cstring>
#include <ctime>
using namespace std;

// 默认可直接访问 HTML 页面列表
const unordered_set<string> HttpRequest::DEFAULT_HTML{
    "/index", "/register", "/login",
    "/welcome", "/video", "/picture",
};

// 默认 HTML 页面标签映射，用于标记登录或注册操作
const unordered_map<string, int> HttpRequest::DEFAULT_HTML_TAG {
    {"/register.html", 0}, {"/login.html", 1},
};

/*
 * Function: Init
 * 逻辑描述: 初始化 HTTP 请求对象，将方法、路径、版本和体部信息置为空，
 *         状态设置为REQUEST_LINE，并清空header和post数据容器。
 */
void HttpRequest::Init() {
    method_ = path_ = version_ = body_ = "";
    state_ = REQUEST_LINE;
    header_.clear();
    post_.clear();
}

/*
 * Function: IsKeepAlive
 * 逻辑描述: 判断当前请求是否为 Keep-Alive 连接。
 *         如果请求报文中 "Connection" 头的值为 "keep-alive" 且 HTTP 版本为 1.1，则返回 true。
 */
bool HttpRequest::IsKeepAlive() const {
    if(header_.count("Connection") == 1) {
        return header_.find("Connection")->second == "keep-alive" && version_ == "1.1";
    }
    return false;
}

/*
 * Function: parse
 * 逻辑描述: 从传入的缓冲区中读取 HTTP 请求报文，
 *         依次解析请求行、头部以及请求体，直至状态变为 FINISH。
 *         同时会在解析完成后输出请求方法、路径和版本信息。
 */
bool HttpRequest::parse(Buffer& buff) {
    const char CRLF[] = "\r\n";
    if(buff.ReadableBytes() <= 0) {
        return false;
    }
    while(buff.ReadableBytes() && state_ != FINISH) {
        // 查找当前行的结束位置
        const char* lineEnd = search(buff.Peek(), buff.BeginWriteConst(), CRLF, CRLF + 2);
        std::string line(buff.Peek(), lineEnd);
        switch(state_) {
            case REQUEST_LINE:
                // 解析请求行
                if(!ParseRequestLine_(line)) {
                    return false;
                }
                // 解析 URL 路径，补全默认文件名
                ParsePath_();
                break;    
            case HEADERS:
                // 解析头部信息
                ParseHeader_(line);
                // 如果缓冲区剩余数据长度小于等于2个字符，则认为头部解析结束
                if(buff.ReadableBytes() <= 2) {
                    state_ = FINISH;
                }
                break;
            case BODY:
                // 解析消息体
                ParseBody_(line);
                break;
            default:
                break;
        }
        // 如果已经遍历到当前写指针位置则退出循环
        if(lineEnd == buff.BeginWrite()) { break; }
        // 移除已经处理的行（包括CRLF）
        buff.RetrieveUntil(lineEnd + 2);
    }
    LOG_DEBUG("[%s], [%s], [%s]", method_.c_str(), path_.c_str(), version_.c_str());
    return true;
}

/*
 * Function: ParsePath_
 * 逻辑描述: 根据请求的 URL 路径进行处理:
 *         如果路径为 "/" 则默认返回 "/index.html"；
 *         如果路径与 DEFAULT_HTML 中的某一项匹配，则在路径后附加 ".html" 后缀。
 */
void HttpRequest::ParsePath_() {
    if(path_ == "/") {
        path_ = "/index.html"; 
    }
    else {
        for(auto &item: DEFAULT_HTML) {
            if(item == path_) {
                path_ += ".html";
                break;
            }
        }
    }
}

/*
 * Function: ParseRequestLine_
 * 逻辑描述: 解析请求行（例如 "GET /index.html HTTP/1.1"），
 *         使用正则表达式提取请求方法、路径和 HTTP 版本，
 *         如果匹配成功则将状态切换到 HEADERS 并返回 true；否则输出错误日志并返回 false。
 */
bool HttpRequest::ParseRequestLine_(const string& line) {
    regex patten("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");
    smatch subMatch;
    if(regex_match(line, subMatch, patten)) {
        method_ = subMatch[1];
        path_ = subMatch[2];
        version_ = subMatch[3];
        state_ = HEADERS;
        return true;
    }
    LOG_ERROR("RequestLine Error");
    return false;
}

/*
 * Function: ParseHeader_
 * 逻辑描述: 解析 HTTP 头部，每一行头部格式为 "Key: Value"，
 *         使用正则表达式提取 key 和 value，并存入 header_ 映射中。
 *         如果一行不符合头部格式，则认为头部结束，状态切换为 BODY。
 */
void HttpRequest::ParseHeader_(const string& line) {
    regex patten("^([^:]*): ?(.*)$");
    smatch subMatch;
    if(regex_match(line, subMatch, patten)) {
        header_[subMatch[1]] = subMatch[2];
    }
    else {
        state_ = BODY;
    }
}

/*
 * Function: ParseBody_
 * 逻辑描述: 将 HTTP 请求体内容存入 body_，并调用 ParsePost_ 处理 POST 数据，
 *         将状态设置为 FINISH，同时输出请求体的调试信息。
 */
void HttpRequest::ParseBody_(const string& line) {
    body_ = line;
    ParsePost_();
    state_ = FINISH;
    LOG_DEBUG("Body:%s, len:%d", line.c_str(), line.size());
}

/*
 * Function: ConverHex
 * 逻辑描述: 将字符 ch (16进制数字)转换为其十进制数值。
 */
int HttpRequest::ConverHex(char ch) {
    if(ch >= 'A' && ch <= 'F') return ch -'A' + 10;
    if(ch >= 'a' && ch <= 'f') return ch -'a' + 10;
    return ch;
}

/*
 * Function: ParsePost_
 * 逻辑描述: 当请求方法为 POST 且内容类型为 application/x-www-form-urlencoded 时，
 *         调用 ParseFromUrlencoded_ 解析表单数据。
 *         同时根据请求的 URL 标签判断是登录还是注册操作，通过 UserVerify 验证用户信息，
 *         并根据验证结果将页面路径设置为 "/welcome.html" 或 "/error.html"。
 */
void HttpRequest::ParsePost_() {
    if(method_ == "POST" && header_["Content-Type"] == "application/x-www-form-urlencoded") {
        ParseFromUrlencoded_();
        if(DEFAULT_HTML_TAG.count(path_)) {
            int tag = DEFAULT_HTML_TAG.find(path_)->second;
            LOG_DEBUG("Tag:%d", tag);
            if(tag == 0 || tag == 1) {
                bool isLogin = (tag == 1);
                if(UserVerify(post_["username"], post_["password"], isLogin)) {
                    path_ = "/welcome.html";
                } 
                else {
                    path_ = "/error.html";
                }
            }
        }
    }   
    if (path_ == "/upload_image" && method_ == "POST") {
        if (!IsUserLoggedIn()) {
            body_ = "{\"success\":false,\"msg\":\"未登录\"}";
            return;
        }
        if(SaveUploadFile("image", "resources/images/", {".jpg", ".jpeg"})) {
            body_ = "{\"success\":true}";
        } else {
            body_ = "{\"success\":false,\"msg\":\"上传失败\"}";
        }
        return;
    }
    if (path_ == "/upload_video" && method_ == "POST") {
        if (!IsUserLoggedIn()) {
            body_ = "{\"success\":false,\"msg\":\"未登录\"}";
            return;
        }
        if(SaveUploadFile("video", "resources/video/", {".mp4"})) {
            body_ = "{\"success\":true}";
        } else {
            body_ = "{\"success\":false,\"msg\":\"上传失败\"}";
        }
        return;
    }
}

/*
 * Function: ParseFromUrlencoded_
 * 逻辑描述: 解析 application/x-www-form-urlencoded 格式的数据，
 *         将请求体中的 key-value 对提取出来存入 post_ 容器，
 *         同时处理 URL 编码中的特殊字符（如 '+' 转换为空格、'%xx' 的十六进制转换）。
 */
void HttpRequest::ParseFromUrlencoded_() {
    if(body_.size() == 0) { return; }

    string key, value;
    int num = 0;
    int n = body_.size();
    int i = 0, j = 0;

    for(; i < n; i++) {
        char ch = body_[i];
        switch (ch) {
        case '=':
            key = body_.substr(j, i - j);
            j = i + 1;
            break;
        case '+':
            body_[i] = ' ';
            break;
        case '%':
            num = ConverHex(body_[i + 1]) * 16 + ConverHex(body_[i + 2]);
            body_[i + 2] = num % 10 + '0';
            body_[i + 1] = num / 10 + '0';
            i += 2;
            break;
        case '&':
            value = body_.substr(j, i - j);
            j = i + 1;
            post_[key] = value;
            LOG_DEBUG("%s = %s", key.c_str(), value.c_str());
            break;
        default:
            break;
        }
    }
    assert(j <= i);
    if(post_.count(key) == 0 && j < i) {
        value = body_.substr(j, i - j);
        post_[key] = value;
    }
}

/*
 * Function: UserVerify
 * 逻辑描述: 验证用户信息：
 *         1. 检查用户名和密码是否为空；
 *         2. 从数据库中查询指定用户名的记录（最多一条）。
 *         3. 如果为登录操作，则比较查询到的密码与传入密码是否一致；
 *            如果为注册操作，则如果查询到记录，则说明用户名已存在，返回验证失败。
 *         4. 对于注册操作且用户名未被占用，则插入新的用户记录。
 *         5. 释放查询结果和数据库连接，返回验证结果 flag。
 */
bool HttpRequest::UserVerify(const string &name, const string &pwd, bool isLogin) {
    if(name == "" || pwd == "") { return false; }
    LOG_INFO("Verify name:%s pwd:%s", name.c_str(), pwd.c_str());
    MYSQL* sql;
    SqlConnRAII(&sql,  SqlConnPool::Instance());
    assert(sql);
    
    bool flag = false;
    unsigned int j = 0;
    char order[256] = { 0 };
    MYSQL_FIELD *fields = nullptr;
    MYSQL_RES *res = nullptr;
    
    if(!isLogin) { flag = true; }
    /* 查询用户及密码 */
    /* 
     * 查询用户及密码的逻辑：
     * 1. 根据传入的用户名构造 SQL 查询语句，查询 user 表中与该用户名匹配的记录（最多一条）。
     * 2. 执行该 SQL 语句，并将结果存储到 res 中。
     * 3. 遍历查询结果：
     *    - 如果是登录行为，则比较数据库返回的密码和传入的 pwd 是否一致；
     *    - 如果是注册行为，则说明用户名已存在，应返回失败（flag = false）。
     * 4. 最后释放查询结果，返回最终的 flag 值决定操作是否成功。
     */

    // 使用 snprintf 构造 SQL 语句，查询指定用户名的记录（限制返回1条）
    snprintf(order, 256, "SELECT username, password FROM user WHERE username='%s' LIMIT 1", name.c_str());
    // 输出调试日志，记录生成的 SQL 语句
    LOG_DEBUG("%s", order);

    // 执行 SQL 查询，若返回非0表示出错
    if(mysql_query(sql, order)) {
        // 出错时释放结果（如果存在），避免内存泄漏
        mysql_free_result(res);
        // 返回 false 表示查询失败
        return false; 
    }

    // 从 MySQL 连接中获取查询结果，并存放在 res 指针中
    res = mysql_store_result(sql);
    // 获取结果集中的字段数量（一般为2个字段：username 和 password）
    j = mysql_num_fields(res);
    // 获取字段的详细描述（字段名、类型等信息），这里未进一步使用
    fields = mysql_fetch_fields(res);

    // 遍历查询结果中的每一行（由于 LIMIT 1，此处最多只有一行）
    while(MYSQL_ROW row = mysql_fetch_row(res)) {
        // 输出调试日志：打印这一行中的用户名和密码
        LOG_DEBUG("MYSQL ROW: %s %s", row[0], row[1]);
        // 将数据库中获取到的密码转换为 C++ 字符串
        string password(row[1]);

        /* 根据操作类型判断：
         * 如果是登录行为，则比较传入密码 pwd 与数据库中 password 是否一致；
         * 如果是注册行为，则本条记录的存在意味着用户名已经被使用，
         * 因此应当将 flag 设置为 false 并记录日志。
         */
        if(isLogin) {
            // 登录操作：如果密码匹配，则验证通过
            if(pwd == password) { 
                flag = true; 
            }
            else {
                // 密码不匹配，验证失败，并记录错误日志
                flag = false;
                LOG_DEBUG("pwd error!");
            }
        } 
        else { 
            // 注册操作：如果查询到记录，说明用户名已存在，注册失败
            flag = false; 
            LOG_DEBUG("user used!");
        }
    }
    // 释放查询结果，防止内存泄漏
    mysql_free_result(res);

    /* 注册行为 且 用户名未被使用*/
    if(!isLogin && flag == true) {
        LOG_DEBUG("regirster!");
        bzero(order, 256);
        snprintf(order, 256,"INSERT INTO user(username, password) VALUES('%s','%s')", name.c_str(), pwd.c_str());
        LOG_DEBUG( "%s", order);
        if(mysql_query(sql, order)) { 
            LOG_DEBUG( "Insert error!");
            flag = false; 
        }
        flag = true;
    }
    // 释放数据库连接
    SqlConnPool::Instance()->FreeConn(sql);
    LOG_DEBUG( "UserVerify success!!");
    return flag;
}

/*
 * Function: IsUserLoggedIn
 * 逻辑描述: 判断用户是否已登录，通过检查请求头中的 Cookie 字段是否包含 username。
 */
bool HttpRequest::IsUserLoggedIn() const {
    auto it = header_.find("Cookie");
    if(it == header_.end()) return false;
    std::string cookies = it->second;
    // 查找 username=xxx
    size_t pos = cookies.find("username=");
    if(pos == std::string::npos) return false;
    // 进一步可校验用户名格式，这里只要有 username 字段就算登录
    return true;
}

/*
 * Function: path
 * 逻辑描述: 获取 HTTP 请求的 URL 路径（const 版本）。
 */
std::string HttpRequest::path() const {
    return path_;
}

/*
 * Function: path (非 const 版本)
 * 逻辑描述: 返回 HTTP 请求的 URL 路径的引用，允许修改。
 */
std::string& HttpRequest::path(){
    return path_;
}

/*
 * Function: method
 * 逻辑描述: 返回 HTTP 请求的方法（GET/POST 等）。
 */
std::string HttpRequest::method() const {
    return method_;
}

/*
 * Function: version
 * 逻辑描述: 返回 HTTP 请求的版本信息（例如 1.1）。
 */
std::string HttpRequest::version() const {
    return version_;
}

/*
 * Function: GetPost (通过 string key)
 * 逻辑描述: 根据 key 从 POST 数据中获取对应的值，如果 key 不存在则返回空字符串。
 */
std::string HttpRequest::GetPost(const std::string& key) const {
    assert(key != "");
    if(post_.count(key) == 1) {
        return post_.find(key)->second;
    }
    return "";
}

/*
 * Function: GetPost (通过 const char* key)
 * 逻辑描述: 根据 key 从 POST 数据中获取对应的值，如果 key 不存在则返回空字符串。
 */
std::string HttpRequest::GetPost(const char* key) const {
    assert(key != nullptr);
    if(post_.count(key) == 1) {
        return post_.find(key)->second;
    }
    return "";
}

/*
 * Function: SaveUploadFile
 * 逻辑描述: 保存上传的文件到指定目录，支持检查文件扩展名。
 */
bool HttpRequest::SaveUploadFile(const std::string& field, const std::string& dir, const std::vector<std::string>& exts) {
    // 解析 multipart/form-data
    // 1. 获取 boundary
    auto it = header_.find("Content-Type");
    if(it == header_.end()) return false;
    std::string contentType = it->second;
    std::string boundary;
    size_t bpos = contentType.find("boundary=");
    if(bpos != std::string::npos) {
        boundary = "--" + contentType.substr(bpos + 9);
    } else {
        return false;
    }

    // 2. 查找文件头部
    size_t pos = body_.find(boundary);
    if(pos == std::string::npos) return false;
    pos += boundary.size() + 2; // 跳过\r\n
    size_t end = body_.find(boundary, pos);
    if(end == std::string::npos) return false;
    std::string part = body_.substr(pos, end - pos);

    // 3. 查找 Content-Disposition
    size_t dispPos = part.find("Content-Disposition:");
    if(dispPos == std::string::npos) return false;
    size_t namePos = part.find("name=\"", dispPos);
    if(namePos == std::string::npos) return false;
    namePos += 6;
    size_t nameEnd = part.find("\"", namePos);
    std::string formName = part.substr(namePos, nameEnd - namePos);
    if(formName != field) return false;

    size_t filenamePos = part.find("filename=\"", dispPos);
    if(filenamePos == std::string::npos) return false;
    filenamePos += 10;
    size_t filenameEnd = part.find("\"", filenamePos);
    std::string filename = part.substr(filenamePos, filenameEnd - filenamePos);

    // 4. 检查扩展名
    bool valid = false;
    for(const auto& ext : exts) {
        if(filename.size() >= ext.size() && filename.substr(filename.size()-ext.size()) == ext) {
            valid = true; break;
        }
    }
    if(!valid) return false;

    // 5. 查找文件内容
    size_t dataPos = part.find("\r\n\r\n", filenameEnd);
    if(dataPos == std::string::npos) return false;
    dataPos += 4;
    size_t dataEnd = part.rfind("\r\n");
    if(dataEnd == std::string::npos || dataEnd <= dataPos) return false;
    std::string filedata = part.substr(dataPos, dataEnd - dataPos);

    // 6. 防止重名，加时间戳
    time_t t = time(nullptr);
    char timestr[32];
    strftime(timestr, sizeof(timestr), "%Y%m%d%H%M%S", localtime(&t));
    std::string saveName = std::string(timestr) + "_" + filename;

    // 7. 创建目录
    mkdir(dir.c_str(), 0777);

    // 8. 保存文件
    std::string savepath = dir + saveName;
    std::ofstream ofs(savepath, std::ios::binary);
    if(!ofs.is_open()) return false;
    ofs.write(filedata.data(), filedata.size());
    ofs.close();
    return true;
}