extern "C" int sdf_c_smoke (void);

int main ()
{
    return sdf_c_smoke () == 0 ? 0 : 1;
}
