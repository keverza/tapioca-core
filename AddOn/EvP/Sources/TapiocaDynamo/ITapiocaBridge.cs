namespace Tapioca;

internal interface ITapiocaBridge
{
    string Call(string command, string paramsJson);
}
